use std::ffi::{c_void, CString};
use std::net::SocketAddr;
use std::sync::Arc;

use futures_util::{future, pin_mut, SinkExt, StreamExt, TryStreamExt};

use anyhow::{bail, Result};

use hyper::{
  header::{HeaderValue, CONNECTION, SEC_WEBSOCKET_ACCEPT, SEC_WEBSOCKET_KEY, SEC_WEBSOCKET_VERSION, UPGRADE},
  upgrade::Upgraded,
  Body, Method, Request, Response, StatusCode, Version,
};

use tokio_tungstenite::WebSocketStream;
use tungstenite::handshake::derive_accept_key;
use tungstenite::protocol::frame::coding::CloseCode;
use tungstenite::protocol::frame::CloseFrame;
use tungstenite::protocol::{Message, Role};

use crate::config::{Config, HttpContent};
use crate::ffi::*;

use include_dir::{include_dir, Dir, File};

static HTML_DIR: Dir<'_> = include_dir!("$CARGO_MANIFEST_DIR/html");

async fn handle_websocket(
  ws_stream: WebSocketStream<Upgraded>,
  request: Request<Body>,
  addr: SocketAddr,
) -> Result<()> {
  info!("{addr}: WebSocket established (uri = {})", request.uri());
  let path = CString::new(request.uri().path())?;

  let wardenclyffe_socket = unsafe { wardenclyffe_create_socket(path.as_ptr()) };
  if wardenclyffe_socket.0.is_null() {
    bail!("{addr}: failed to create socket");
  }

  let (mut outgoing, incoming) = ws_stream.split();
  let supports_read = unsafe { wardenclyffe_supports_read(wardenclyffe_socket) };
  let supports_write = unsafe { wardenclyffe_supports_write(wardenclyffe_socket) };

  let incoming = incoming.try_for_each(|msg| {
    let msg = msg.to_text().unwrap();
    if supports_write {
      debug!("{addr}: received message: {}", msg);
      let msg_bytes = msg.as_bytes();

      // TODO: The lifetime of the socket seems dubious here...
      let result = unsafe {
        wardenclyffe_write(
          wardenclyffe_socket,
          msg_bytes.as_ptr() as *const c_void,
          msg_bytes.len(),
        )
      };
      if result {
        future::ok(())
      } else {
        future::err(tungstenite::Error::ConnectionClosed)
      }
    } else {
      info!("{addr}: received unhandled message: {}", msg);
      future::ok(())
    }
  });

  let outgoing = tokio::spawn(async move {
    if supports_read {
      loop {
        let reads = {
          tokio::task::spawn_blocking(move || unsafe { wardenclyffe_read(wardenclyffe_socket) })
            .await
            .expect("failed to join")
        };

        if reads.read_count < 0 {
          error!("{addr}: WardenclyffeSocket::read failed: rc = {}", reads.read_count);
          let _ = outgoing
            .send(Message::Close(Some(CloseFrame {
              code: CloseCode::Error,
              reason: "read failed".into(),
            })))
            .await;
          return;
        } else if reads.read_count == 0 {
          info!("{addr}: WardenclyffeSocket hit EOF");
          let _ = outgoing
            .send(Message::Close(Some(CloseFrame {
              code: CloseCode::Normal,
              reason: "EOF".into(),
            })))
            .await;
          return;
        }

        let reads = unsafe { std::slice::from_raw_parts(reads.reads, reads.read_count as usize) };
        for read in reads {
          let buf = unsafe { std::slice::from_raw_parts(read.data as *const u8, read.size) }.to_vec();
          let result = if read.oob != 0 {
            let buf_str = unsafe { String::from_utf8_unchecked(buf) };
            outgoing.send(Message::Text(buf_str))
          } else {
            outgoing.send(Message::Binary(buf))
          };
          if let Err(e) = result.await {
            error!("{addr}: failed to send: {e}");
            return;
          }
        }
      }
    } else {
      future::pending().await
    }
  });

  pin_mut!(incoming, outgoing);
  future::select(incoming, outgoing).await;

  info!("{addr}: disconnected");
  unsafe {
    wardenclyffe_destroy_socket(wardenclyffe_socket);
  }

  Ok(())
}

fn get_http_content(http_content: &HttpContent, path: &str) -> Option<Vec<u8>> {
  match http_content {
    HttpContent::Embedded => HTML_DIR.get_file(path).map(File::contents).map(<[u8]>::to_vec),
    HttpContent::Path(base_path) => std::fs::read(base_path.join(path)).ok(),
  }
}

pub async fn handle_request(config: Arc<Config>, mut req: Request<Body>, addr: SocketAddr) -> Result<Response<Body>> {
  let upgrade = HeaderValue::from_static("Upgrade");
  let websocket = HeaderValue::from_static("websocket");
  let headers = req.headers();
  let key = headers.get(SEC_WEBSOCKET_KEY);
  let derived = key.map(|k| derive_accept_key(k.as_bytes()));

  if req.method() == Method::GET
    && req.version() >= Version::HTTP_11
    && headers
      .get(CONNECTION)
      .and_then(|h| h.to_str().ok())
      .map(|h| {
        h.split(|c| c == ' ' || c == ',')
          .any(|p| p.eq_ignore_ascii_case(upgrade.to_str().unwrap()))
      })
      .unwrap_or(false)
    && headers
      .get(UPGRADE)
      .and_then(|h| h.to_str().ok())
      .map(|h| h.eq_ignore_ascii_case("websocket"))
      .unwrap_or(false)
    && headers.get(SEC_WEBSOCKET_VERSION).map(|h| h == "13").unwrap_or(false)
    && key.is_some()
  {
    let ver = req.version();
    tokio::task::spawn(async move {
      match hyper::upgrade::on(&mut req).await {
        Ok(upgraded) => {
          if let Err(e) = handle_websocket(
            WebSocketStream::from_raw_socket(upgraded, Role::Server, None).await,
            req,
            addr,
          )
          .await
          {
            error!("failed to handle websocket: {e:?}");
          }
        }
        Err(e) => error!("upgrade error: {}", e),
      }
    });
    let mut res = Response::new(Body::empty());
    *res.status_mut() = StatusCode::SWITCHING_PROTOCOLS;
    *res.version_mut() = ver;
    res.headers_mut().append(CONNECTION, upgrade);
    res.headers_mut().append(UPGRADE, websocket);
    res
      .headers_mut()
      .append(SEC_WEBSOCKET_ACCEPT, derived.unwrap().parse().unwrap());
    return Ok(res);
  }

  info!("HTTP request for {}", req.uri());
  let path = req.uri().path();
  if !path.starts_with('/') {
    let mut response = Response::new(Body::from("Bad request"));
    *response.status_mut() = StatusCode::BAD_REQUEST;
    return Ok(response);
  }

  let http_content = config.http_content.as_ref().unwrap();

  let mut path = &path[1..];
  if let Some(file) = get_http_content(http_content, path) {
    return Ok(Response::new(Body::from(file)));
  }

  // Assume it's a directory, look for index.html.
  while path.ends_with('/') {
    path = &path[..path.len() - 1];
  }

  let index_path = format!("{}/{}", path, "index.html");
  if let Some(file) = get_http_content(http_content, &index_path) {
    return Ok(Response::new(Body::from(file)));
  }

  let mut response = Response::new(Body::from(format!("File not found: {path}")));
  *response.status_mut() = StatusCode::NOT_FOUND;
  Ok(response)
}
