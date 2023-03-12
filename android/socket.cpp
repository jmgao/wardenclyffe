#include "wardenclyffe/android/socket.h"

#include <thread>

#include <android-base/strings.h>
#include <binder/IPCThreadState.h>

#include "wardenclyffe/android/socket.h"
#include "wardenclyffe/android/video/video.h"
#include "wardenclyffe/wardenclyffe.h"

static std::once_flag once;

WardenclyffeSocket wardenclyffe_create_socket(const char* path_str) {
  std::call_once(once, []() {
    // Start Binder thread pool.  MediaCodec needs to be able to receive
    // messages from mediaserver.
    android::sp<android::ProcessState> self = android::ProcessState::self();
    self->startThreadPool();
  });

  std::string_view path(path_str);
  if (android::base::ConsumePrefix(&path, "/video/")) {
    return VideoSocket::Create(path);
  } else if (android::base::ConsumePrefix(&path, "/audio/")) {
    // TODO
  } else if (android::base::ConsumePrefix(&path, "/input/")) {
    // TODO
  }

  return nullptr;
}

void wardenclyffe_destroy_socket(WardenclyffeSocket socket) {
  auto s = static_cast<Socket*>(socket);
  s->Destroy();
  delete s;
}

bool wardenclyffe_supports_read(WardenclyffeSocket socket) {
  return static_cast<Socket*>(socket)->SupportsRead();
}

WardenclyffeReads wardenclyffe_read(WardenclyffeSocket socket) {
  return static_cast<Socket*>(socket)->Read();
}

bool wardenclyffe_supports_write(WardenclyffeSocket socket) {
  return static_cast<Socket*>(socket)->SupportsWrite();
}

bool wardenclyffe_write(WardenclyffeSocket socket, const void* data, size_t len) {
  return static_cast<Socket*>(socket)->Write(data, len);
}
