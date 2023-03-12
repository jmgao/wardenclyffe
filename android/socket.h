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
  virtual void Destroy() {}

  virtual WardenclyffeReads Read() { return {.reads = nullptr, .read_count = 0}; }
  virtual bool SupportsRead() { return false; }

  virtual bool Write([[maybe_unused]] const void* data, [[maybe_unused]] size_t len) {
    return false;
  }
  virtual bool SupportsWrite() { return false; }
};
