use std::ffi::{c_char, CStr};
use std::path::PathBuf;

use clap::Parser;

use crate::{
  config::{Config, TLS},
  Server,
};

#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
  /// Path of configuration file to use.
  #[arg(short = 'C', default_value = "/data/vendor_de/0/wardenclyffe/config.json")]
  config: PathBuf,

  #[arg(short = 'p')]
  port: Option<u16>,

  #[arg(short = 'c')]
  cert: Option<PathBuf>,

  #[arg(short = 'k')]
  private_key: Option<PathBuf>,

  #[arg(long, default_value_t = false)]
  dump_config: bool,
}

#[export_name = "main"]
extern "C" fn wardenclyffe_main(argc: i32, argv: *mut *mut c_char) -> i32 {
  let args = unsafe {
    let slice = std::slice::from_raw_parts(argv, argc as usize);
    slice
      .iter()
      .map(|p| CStr::from_ptr(*p).to_str().expect("argument not UTF-8"))
  };
  let args = Args::parse_from(args);

  let mut config = match std::fs::read(&args.config) {
    Ok(config_file) => serde_json::from_slice(&config_file).expect("failed to parse config file"),

    Err(err) => {
      eprintln!("failed to open '{:?}', falling back to defaults: {err}", args.config);
      Config::default()
    }
  };

  config.port = args.port.or(config.port);

  match (args.cert, args.private_key) {
    (Some(c), Some(k)) => {
      config.tls = Some(TLS::Certificate {
        cert_path: c,
        private_key_path: k,
      })
    }

    (None, None) => {}

    _ => {
      panic!("--cert must be specified with --private_key");
    }
  }

  config = config.populate_defaults();
  if args.dump_config {
    println!("{}", serde_json::to_string_pretty(&config).unwrap());
  }

  let server = Server::from_config(config);
  server.run().expect("failed to serve");
  0
}
