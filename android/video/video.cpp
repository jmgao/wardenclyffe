#include "wardenclyffe/android/video/video.h"

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/thread_annotations.h>
#include <binder/IPCThreadState.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <media/MediaCodecBuffer.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <mediadrm/ICrypto.h>
#include <ui/DisplayState.h>
#include <utils/String8.h>

#include "wardenclyffe/android/socket.h"

using namespace android;

static inline uint32_t floorToEven(uint32_t num) {
  return num & ~1;
}

VideoSocket::~VideoSocket() {
  if (display_) {
    SurfaceComposerClient::destroyDisplay(display_);
  }
}

VideoSocket* VideoSocket::Create(std::string_view path) {
  VideoSocket* result = nullptr;
  if (android::base::ConsumePrefix(&path, "h264/")) {
    result = new H264Socket();
  }

  if (result) {
    if (!result->Initialize()) {
      delete result;
    }
  }

  return result;
}

WardenclyffeReads VideoSocket::Read() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this]() {
    if (!running_) return true;
    if (reads_.empty()) {
      return !frames_.empty();
    } else {
      return frames_.size() > 1;
    }
  });

  WardenclyffeReads result = {.reads = nullptr, .read_count = -1};

  if (!running_) {
    return result;
  }

  if (!reads_.empty()) {
    reads_.clear();
    frames_.pop_front();
    if (emit_descriptors_) {
      descriptions_.pop_front();
    }
  }

  const Frame& frame = frames_.at(0);

  if (emit_descriptors_) {
    const char* frame_type = nullptr;
    switch (frame.type) {
      case FrameType::Description:
        frame_type = "config";
        break;

      case FrameType::Keyframe:
        frame_type = "key";
        break;

      case FrameType::Interframe:
        frame_type = "delta";
        break;
    };

    descriptions_.push_back(android::base::StringPrintf(
        "{\"type\":\"%s\",\"timestamp\": %" PRId64 "}", frame_type, frame.timestamp));

    reads_.push_back(WardenclyffeRead{
        .data = descriptions_.back().data(),
        .size = descriptions_.back().size(),
        .oob = true,
    });
  }
  reads_.push_back(WardenclyffeRead{
      .data = frame.data.data(),
      .size = frame.data.size(),
      .oob = false,
  });
  result.reads = reads_.data();
  result.read_count = reads_.size();
  return result;
}

bool VideoSocket::fetchDisplayParameters() {
  std::optional<PhysicalDisplayId> displayId = SurfaceComposerClient::getInternalDisplayId();
  if (!displayId) {
    LOG(ERROR) << "Failed to get ID for internal display";
    return false;
  }

  sp<IBinder> display = SurfaceComposerClient::getPhysicalDisplayToken(*displayId);
  if (display == nullptr) {
    LOG(ERROR) << "Failed to get display";
    return false;
  }

  status_t err = SurfaceComposerClient::getDisplayState(display, &display_state_);
  if (err != NO_ERROR) {
    LOG(ERROR) << "Failed to get display state";
    return false;
  }

  err = SurfaceComposerClient::getActiveDisplayMode(display, &display_mode_);
  if (err != NO_ERROR) {
    LOG(ERROR) << "Failed to get display mode";
    return false;
  }

  if (video_width_ == 0) {
    video_width_ = floorToEven(display_state_.layerStackSpaceRect.getWidth()) * 0.5;
  }
  if (video_height_ == 0) {
    video_height_ = floorToEven(display_state_.layerStackSpaceRect.getHeight()) * 0.5;
  }

  return true;
}

void VideoSocket::setDisplayProjection(SurfaceComposerClient::Transaction& t, sp<IBinder> display,
                                       const ui::DisplayState& displayState) {
  // Set the region of the layer stack we're interested in, which in our case is "all of it".
  Rect layerStackRect(displayState.layerStackSpaceRect);

  // We need to preserve the aspect ratio of the display.
  float displayAspect = layerStackRect.getHeight() / static_cast<float>(layerStackRect.getWidth());

  // Set the way we map the output onto the display surface (which will
  // be e.g. 1280x720 for a 720p video).  The rect is interpreted
  // post-rotation, so if the display is rotated 90 degrees we need to
  // "pre-rotate" it by flipping width/height, so that the orientation
  // adjustment changes it back.
  //
  // We might want to encode a portrait display as landscape to use more
  // of the screen real estate.  (If players respect a 90-degree rotation
  // hint, we can essentially get a 720x1280 video instead of 1280x720.)
  // In that case, we swap the configured video width/height and then
  // supply a rotation value to the display projection.
  uint32_t videoWidth = video_width_;
  uint32_t videoHeight = video_height_;
  uint32_t outWidth, outHeight;

  if (videoHeight > (uint32_t)(videoWidth * displayAspect)) {
    // limited by narrow width; reduce height
    outWidth = videoWidth;
    outHeight = (uint32_t)(videoWidth * displayAspect);
  } else {
    // limited by short height; restrict width
    outHeight = videoHeight;
    outWidth = (uint32_t)(videoHeight / displayAspect);
  }
  uint32_t offX, offY;
  offX = (videoWidth - outWidth) / 2;
  offY = (videoHeight - outHeight) / 2;
  Rect displayRect(offX, offY, offX + outWidth, offY + outHeight);

  t.setDisplayProjection(display, ui::ROTATION_0, layerStackRect, displayRect);
}

bool VideoSocket::createVirtualDisplay() {
  display_ = SurfaceComposerClient::createDisplay(String8("wardenclyffe"), false /*secure*/);
  if (!display_) {
    LOG(ERROR) << "Failed to create virtual display";
    return false;
  }
  return true;
}

bool VideoSocket::prepareVirtualDisplay() {
  SurfaceComposerClient::Transaction t;
  t.setDisplaySurface(display_, codec_surface_);
  setDisplayProjection(t, display_, display_state_);
  t.setDisplayLayerStack(display_, display_state_.layerStack);
  t.apply();
  return true;
}
