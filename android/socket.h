#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>
#include <unistd.h>

#include <utility>

#include <android-base/unique_fd.h>

#include "wardenclyffe/wardenclyffe.h"

struct Socket {
  virtual ~Socket() = default;
  virtual WardenclyffeReads Read() = 0;
};

struct CaptureFactory {
  Socket* CreateAudio();
  Socket* CreateVideo();
};
