extern crate cbindgen;

fn main() {
  let crate_dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
  let config = cbindgen::Config::from_root_or_default(&crate_dir);
  match cbindgen::Builder::new()
    .with_config(config)
    .with_crate(crate_dir)
    .generate()
  {
    Ok(bindings) => {
      bindings.write_to_file("include/wardenclyffe/wardenclyffe.h");
    }

    Err(err) => {
      eprintln!("Unable to generate bindings: {err}");
    }
  }
}
