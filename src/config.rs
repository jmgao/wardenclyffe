use std::path::PathBuf;

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, PartialEq)]
pub enum TLS {
  Disabled,
  SelfSigned,
  Certificate {
    cert_path: PathBuf,
    private_key_path: PathBuf,
  },
}

#[derive(Serialize, Deserialize, PartialEq)]
pub enum HttpContent {
  Embedded,
  Path(PathBuf),
}

#[derive(Serialize, Deserialize, Default)]
pub struct Config {
  pub port: Option<u16>,
  pub tls: Option<TLS>,
  pub http_content: Option<HttpContent>,
}

impl Config {
  pub fn populate_defaults(mut self) -> Self {
    self.tls = self.tls.or(Some(TLS::SelfSigned));
    self.port = self.port.or(self.tls.as_ref().map(|_| 8443).or(Some(8443)));
    self.http_content = self.http_content.or(Some(HttpContent::Embedded));
    self
  }
}
