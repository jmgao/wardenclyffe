#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>
#include <unistd.h>

#include <string_view>
#include <utility>

#include <android-base/unique_fd.h>

#include "wardenclyffe/wardenclyffe.h"

struct Socket {
  virtual ~Socket() = default;
  virtual void Destroy() = 0;

  virtual WardenclyffeReads Read() = 0;
};
