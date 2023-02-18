use std::ffi::CString;
use std::net::SocketAddr;

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

use include_dir::{include_dir, Dir};

use crate::ffi::*;

static HTML_DIR: Dir<'_> = include_dir!("$CARGO_MANIFEST_DIR/html");

async fn handle_websocket(
  ws_stream: WebSocketStream<Upgraded>,
  request: Request<Body>,
  addr: SocketAddr,
) -> Result<()> {
  eprintln!("{addr}: WebSocket established (uri = {})", request.uri());
  let path = CString::new(request.uri().path())?;

  let wardenclyffe_socket = unsafe { wardenclyffe_create_socket(path.as_ptr()) };
  if wardenclyffe_socket.0.is_null() {
    bail!("{addr}: failed to create socket");
  }

  let (mut outgoing, incoming) = ws_stream.split();
  let incoming = incoming.try_for_each(|msg| {
    eprintln!("{addr}: received message: {}", msg.to_text().unwrap());
    future::ok(())
  });

  let outgoing = {
    tokio::spawn(async move {
      loop {
        let reads = {
          tokio::task::spawn_blocking(move || unsafe { wardenclyffe_read(wardenclyffe_socket) })
            .await
            .expect("failed to join")
        };

        if reads.read_count < 0 {
          eprintln!("{addr}: WardenclyffeSocket::read failed: rc = {}", reads.read_count);
          let _ = outgoing
            .send(Message::Close(Some(CloseFrame {
              code: CloseCode::Error,
              reason: "read failed".into(),
            })))
            .await;
          return;
        } else if reads.read_count == 0 {
          eprintln!("{addr}: WardenclyffeSocket hit EOF");
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
            eprintln!("{addr}: failed to send: {e}");
            return;
          }
        }
      }
    })
  };

  pin_mut!(incoming, outgoing);
  future::select(incoming, outgoing).await;

  eprintln!("{addr}: disconnected");
  unsafe {
    wardenclyffe_destroy_socket(wardenclyffe_socket);
  }

  Ok(())
}

pub async fn handle_request(mut req: Request<Body>, addr: SocketAddr) -> Result<Response<Body>> {
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
            eprintln!("failed to handle websocket: {e:?}");
          }
        }
        Err(e) => eprintln!("upgrade error: {}", e),
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

  eprintln!("HTTP request for {}", req.uri());
  let path = req.uri().path();
  if !path.starts_with('/') {
    let mut response = Response::new(Body::from("Bad request"));
    *response.status_mut() = StatusCode::BAD_REQUEST;
    return Ok(response);
  }

  let path = &path[1..];
  let path = if path.is_empty() { "index.html" } else { path };

  if let Some(file) = HTML_DIR.get_file(path) {
    Ok(Response::new(Body::from(file.contents())))
  } else {
    let mut response = Response::new(Body::from(format!("File not found: {path}")));
    *response.status_mut() = StatusCode::NOT_FOUND;
    Ok(response)
  }
}
