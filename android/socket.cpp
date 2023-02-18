#include "wardenclyffe/wardenclyffe.h"

#include <thread>

#include <binder/IPCThreadState.h>

#include "android/socket.h"

static std::once_flag once;

static CaptureFactory factory;

WardenclyffeSocket wardenclyffe_create_socket(const char* path) {
  std::call_once(once, []() {
    // Start Binder thread pool.  MediaCodec needs to be able to receive
    // messages from mediaserver.
    android::sp<android::ProcessState> self = android::ProcessState::self();
    self->startThreadPool();
  });

  return factory.CreateVideo();
}

void wardenclyffe_destroy_socket(WardenclyffeSocket socket) {
  delete static_cast<Socket*>(socket);
}

WardenclyffeReads wardenclyffe_read(WardenclyffeSocket socket) {
  return static_cast<Socket*>(socket)->Read();
}
