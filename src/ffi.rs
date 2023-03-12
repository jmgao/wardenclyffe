use std::ffi::{c_char, c_void};

#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct WardenclyffeSocket(pub *mut c_void);

unsafe impl Sync for WardenclyffeSocket {}
unsafe impl Send for WardenclyffeSocket {}

#[repr(C)]
pub struct WardenclyffeRead {
  pub data: *const c_void,
  pub size: usize,
  pub oob: u8,
}

unsafe impl Sync for WardenclyffeRead {}
unsafe impl Send for WardenclyffeRead {}

#[repr(C)]
pub struct WardenclyffeReads {
  pub reads: *const WardenclyffeRead,
  pub read_count: isize,
}

unsafe impl Sync for WardenclyffeReads {}
unsafe impl Send for WardenclyffeReads {}

extern "C" {
  pub fn wardenclyffe_create_socket(path: *const c_char) -> WardenclyffeSocket;
  pub fn wardenclyffe_destroy_socket(socket: WardenclyffeSocket) -> ();

  pub fn wardenclyffe_supports_read(socket: WardenclyffeSocket) -> bool;
  pub fn wardenclyffe_read(socket: WardenclyffeSocket) -> WardenclyffeReads;

  pub fn wardenclyffe_supports_write(socket: WardenclyffeSocket) -> bool;
  pub fn wardenclyffe_write(socket: WardenclyffeSocket, data: *const c_void, len: usize) -> bool;
}
