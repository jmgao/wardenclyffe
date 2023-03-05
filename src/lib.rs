use std::fs::File;
use std::io::{self, BufReader};
use std::net::SocketAddr;
use std::sync::Arc;

use anyhow::{bail, Result};
use hyper::{
  server::conn::{AddrIncoming, AddrStream},
  service::{make_service_fn, service_fn},
};
use rustls_pemfile::Item;

#[macro_use]
extern crate log;

mod cli;
mod config;
mod ffi;
mod server;
mod tls;

use config::Config;
use server::*;
use tls::{TlsAcceptor, TlsStream};

pub struct Server {
  config: Config,
}

#[derive(Default)]
pub struct ServerBuilder {
  config: Config,
}

impl ServerBuilder {
  pub fn new() -> Self {
    Default::default()
  }

  pub fn from_config(config: Config) -> Self {
    ServerBuilder { config }
  }

  pub fn port(mut self, p: u16) -> Self {
    self.config.port = Some(p);
    self
  }

  pub fn build(self) -> Server {
    Server { config: self.config }
  }
}

impl Server {
  pub fn builder() -> ServerBuilder {
    ServerBuilder::new()
  }

  pub fn from_config(config: Config) -> Self {
    ServerBuilder::from_config(config).build()
  }

  pub fn get_acme_certs(&self) -> Result<(rustls::Certificate, rustls::PrivateKey)> {
    unimplemented!();
  }

  pub fn load_certs(config: &Config) -> Result<rustls::ServerConfig> {
    let (cert_chain, key) = match config.tls.as_ref().unwrap_or(&config::TLS::SelfSigned) {
      config::TLS::SelfSigned => {
        let self_signed = rcgen::generate_simple_self_signed(vec!["*".into()]).unwrap();
        let cert = rustls::Certificate(self_signed.serialize_der()?);
        let key = rustls::PrivateKey(self_signed.serialize_private_key_der());
        (vec![cert], key)
      }

      config::TLS::Certificate {
        cert_path,
        private_key_path,
      } => {
        let mut cert_file = BufReader::new(File::open(cert_path)?);
        let cert_chain = rustls_pemfile::certs(&mut cert_file)?
          .iter()
          .map(|vec| rustls::Certificate(vec.clone()))
          .collect();

        let mut key_file = BufReader::new(File::open(private_key_path)?);
        let keys = rustls_pemfile::read_all(&mut key_file)?;
        if keys.len() != 1 {
          panic!("failed to find key");
        }

        if let Item::PKCS8Key(key) = &keys[0] {
          let key = rustls::PrivateKey(key.clone());
          (cert_chain, key)
        } else {
          panic!("failed to find key");
        }
      }

      config::TLS::Disabled => {
        bail!("TLS not enabled");
      }
    };
    let mut cfg = rustls::ServerConfig::builder()
      .with_safe_defaults()
      .with_no_client_auth()
      .with_single_cert(cert_chain, key)
      .unwrap();

    cfg.alpn_protocols = vec![b"h2".to_vec(), b"http/1.1".to_vec()];
    Ok(cfg)
  }

  pub fn run(self) -> Result<()> {
    android_logger::init_once(android_logger::Config::default().with_max_level(log::LevelFilter::Info));

    let config = self.config.populate_defaults();
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(async move {
      let addr = format!("0.0.0.0:{}", config.port.unwrap())
        .parse::<SocketAddr>()
        .unwrap();
      if config.tls == Some(config::TLS::Disabled) {
        let service = make_service_fn(move |conn: &AddrStream| {
          let remote_addr = conn.remote_addr();
          let service = service_fn(move |req| handle_request(req, remote_addr));
          async move { Ok::<_, io::Error>(service) }
        });

        let server = hyper::Server::bind(&addr).serve(service);
        server.await
      } else {
        let tls_cfg = Arc::new(Server::load_certs(&config).expect("failed to load TLS certs"));
        let service = make_service_fn(move |conn: &TlsStream| {
          let remote_addr = conn.remote_addr();
          let service = service_fn(move |req| handle_request(req, remote_addr));
          async move { Ok::<_, io::Error>(service) }
        });
        let incoming = AddrIncoming::bind(&addr).unwrap();

        let server = hyper::Server::builder(TlsAcceptor::new(tls_cfg, incoming)).serve(service);
        server.await
      }
    })?;
    Ok(())
  }
}
