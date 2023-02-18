/*
 * Copyright 2013 The Android Open Source Project
 *           2023 Josh Gao <josh@jmgao.dev>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Derived from frameworks/av/cmds/screenrecord/screenrecord.cpp

#include "android/socket.h"

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
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
#include <utils/StrongPointer.h>

using namespace android;

static constexpr char kMimeTypeAvc[] = "video/avc";

static inline uint32_t floorToEven(uint32_t num) {
  return num & ~1;
}

enum class FrameType { Description, Keyframe, Interframe };

struct Frame {
  std::vector<char> data;
  FrameType type;
  int64_t timestamp;
};

struct VideoSocket : public Socket {
  VideoSocket() = default;
  explicit VideoSocket(bool emit_descriptors) : emit_descriptors_(emit_descriptors) {}

  ~VideoSocket() {
    if (encoder_thread_) {
      stopEncoder();
    }

    if (codec_) {
      codec_->stop();
      codec_->release();
    }

    if (display_) {
      SurfaceComposerClient::destroyDisplay(display_);
    }
  }

  bool Initialize() {
    return fetchDisplayParameters() && prepareEncoder() && createVirtualDisplay() &&
           prepareVirtualDisplay() && startEncoder();
  }

  WardenclyffeReads Read() final {
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

 private:
  bool fetchDisplayParameters() {
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

  bool prepareEncoder() {
    sp<AMessage> format = new AMessage();
    format->setInt32(KEY_WIDTH, video_width_);
    format->setInt32(KEY_HEIGHT, video_height_);
    format->setString(KEY_MIME, kMimeTypeAvc);
    format->setInt32(KEY_COLOR_FORMAT, OMX_COLOR_FormatAndroidOpaque);
    format->setInt32(KEY_BIT_RATE, video_bitrate_);
    format->setFloat(KEY_FRAME_RATE, video_framerate_);
    format->setInt32(KEY_I_FRAME_INTERVAL, video_iframe_interval_);
    format->setInt32(KEY_MAX_B_FRAMES, video_bframes_);
    format->setInt32(KEY_PROFILE, AVCProfileConstrainedBaseline);
    format->setInt32(KEY_LEVEL, AVCLevel41);

    looper_ = new ALooper();
    looper_->setName("wardenclyffe_looper");
    looper_->start();

    codec_ = MediaCodec::CreateByType(looper_, kMimeTypeAvc, true);
    if (!codec_) {
      LOG(ERROR) << "Failed to create codec instance";
      return false;
    }

    status_t err = codec_->configure(format, NULL, NULL, MediaCodec::CONFIGURE_FLAG_ENCODE);
    if (err != NO_ERROR) {
      LOG(ERROR) << "Failed to configure codec at " << video_width_ << "x" << video_height_
                 << " (err = " << err << ")";
      codec_->release();
      return err;
    }

    err = codec_->createInputSurface(&buffer_producer__);
    if (err != NO_ERROR) {
      LOG(ERROR) << "Failed to create encoder input surface (err = " << err << ")";
      codec_->release();
      return false;
    }

    err = codec_->start();
    if (err != NO_ERROR) {
      LOG(ERROR) << "Failed to start codec (err = " << err << ")";
      codec_->release();
      return false;
    }

    LOG(INFO) << "Codec instantiated";
    return true;
  }

  void setDisplayProjection(SurfaceComposerClient::Transaction& t, sp<IBinder> display,
                            const ui::DisplayState& displayState) {
    // Set the region of the layer stack we're interested in, which in our case is "all of it".
    Rect layerStackRect(displayState.layerStackSpaceRect);

    // We need to preserve the aspect ratio of the display.
    float displayAspect =
        layerStackRect.getHeight() / static_cast<float>(layerStackRect.getWidth());

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

  bool createVirtualDisplay() {
    display_ = SurfaceComposerClient::createDisplay(String8("wardenclyffe"), false /*secure*/);
    if (!display_) {
      LOG(ERROR) << "Failed to create virtual display";
      return false;
    }
    return true;
  }

  bool prepareVirtualDisplay() {
    SurfaceComposerClient::Transaction t;
    t.setDisplaySurface(display_, buffer_producer__);
    setDisplayProjection(t, display_, display_state_);
    t.setDisplayLayerStack(display_, display_state_.layerStack);
    t.apply();
    return true;
  }

  bool startEncoder() {
    CHECK(!running_);
    running_ = true;
    encoder_thread_ = std::thread([this]() {
      Vector<sp<MediaCodecBuffer>> buffers;
      status_t err = codec_->getOutputBuffers(&buffers);
      if (err != NO_ERROR) {
        LOG(FATAL) << "failed to get output buffers (err = " << err << ")";
      }

      bool skip = false;
      while (running_) {
        size_t bufIndex, offset, size;
        int64_t ptsUsec;
        uint32_t flags;

        static constexpr int kTimeout = 250000;
        err = codec_->dequeueOutputBuffer(&bufIndex, &offset, &size, &ptsUsec, &flags, kTimeout);
        switch (err) {
          case NO_ERROR:
            if (size != 0) {
              LOG(VERBOSE) << "Got data in buffer " << bufIndex << ", size = " << size
                           << ", pts = " << ptsUsec;

              // Check orientation, update if it has changed.
              //
              // Polling for changes is inefficient and wrong, but the
              // useful stuff is hard to get at without a Dalvik VM.
              ui::DisplayState currentDisplayState;
              err = SurfaceComposerClient::getDisplayState(display_, &currentDisplayState);
              if (err != NO_ERROR) {
                LOG(WARNING) << "getDisplayState failed (err = " << err << ")";
              } else if (display_state_.orientation != currentDisplayState.orientation ||
                         display_state_.layerStack != currentDisplayState.layerStack) {
                LOG(INFO) << "Updating display state";
                prepareVirtualDisplay();
              }

              Frame frame;
              if (flags & BUFFER_FLAG_CODEC_CONFIG) {
                frame.type = FrameType::Description;
              } else if (flags & BUFFER_FLAG_KEY_FRAME) {
                frame.type = FrameType::Keyframe;
              } else {
                frame.type = FrameType::Interframe;
              }

              char* p = reinterpret_cast<char*>(buffers[bufIndex]->data());
              if (frame.type == FrameType::Description) {
                partial_frame_.assign(p, p + size);
              } else {
                std::vector<char> data = std::move(partial_frame_);

                if (!skip) {
                  data.insert(data.end(), p, p + size);
                  frame.data = std::move(data);
                  frame.timestamp = ptsUsec;

                  {
                    std::lock_guard<std::mutex> lock(mutex_);
                    frames_.push_back(std::move(frame));
                  }
                  cv_.notify_one();
                }
                skip = !skip;
              }
            }
            err = codec_->releaseOutputBuffer(bufIndex);

            if (err != NO_ERROR) {
              LOG(ERROR) << "Unable to release output buffer: (err = " << err << ")";
              goto exit;
            }

            if ((flags & MediaCodec::BUFFER_FLAG_EOS) != 0) {
              LOG(ERROR) << "Received end of stream from surfaceflinger";
              goto exit;
            }
            break;

          case -EAGAIN:  // INFO_TRY_AGAIN_LATER
            LOG(VERBOSE) << "Got -EAGAIN, looping";
            break;

          case android::INFO_FORMAT_CHANGED:  // INFO_OUTPUT_FORMAT_CHANGED
            LOG(VERBOSE) << "Encoder format changed";
            break;

          case android::INFO_OUTPUT_BUFFERS_CHANGED:  // INFO_OUTPUT_BUFFERS_CHANGED
            // Not expected for an encoder; handle it anyway.
            LOG(INFO) << "Encoder buffers changed";
            err = codec_->getOutputBuffers(&buffers);
            if (err != NO_ERROR) {
              LOG(ERROR) << "Unable to get new output buffers (err = " << err << ")";
              goto exit;
            }
            break;

          case INVALID_OPERATION:
            LOG(ERROR) << "dequeueOutputBuffer returned INVALID_OPERATION";
            goto exit;

          default:
            LOG(ERROR) << "Got weird result " << err << " from dequeueOutputBuffer";
            goto exit;
        }
      }

    exit:
      LOG(INFO) << "Encoder stopping (request = " << !running_ << ")";
      running_ = false;
    });
    return true;
  }

  void stopEncoder() {
    running_ = false;
    if (encoder_thread_) {
      encoder_thread_->join();
      encoder_thread_.reset();
    }
    cv_.notify_all();
  }

 private:
  bool emit_descriptors_ = false;

  ui::DisplayState display_state_;
  ui::DisplayMode display_mode_;

  uint32_t video_width_ = 0;
  uint32_t video_height_ = 0;
  uint32_t video_bitrate_ = 20'000'000;

  float video_framerate_ = 0;
  uint32_t video_iframe_interval_ = 10;
  uint32_t video_bframes_ = 0;

  sp<ALooper> looper_;
  sp<MediaCodec> codec_;
  sp<IGraphicBufferProducer> buffer_producer__;
  sp<IBinder> display_;

  std::atomic<bool> running_ = false;
  std::optional<std::thread> encoder_thread_;

  std::mutex mutex_;
  std::condition_variable cv_;

  std::deque<Frame> frames_ GUARDED_BY(mutex_);
  std::vector<char> partial_frame_ GUARDED_BY(mutex_);
  std::deque<std::string> descriptions_ GUARDED_BY(mutex_);
  std::vector<WardenclyffeRead> reads_ GUARDED_BY(mutex_);
};

Socket* CaptureFactory::CreateVideo() {
  VideoSocket* socket = new VideoSocket();
  if (!socket->Initialize()) {
    delete socket;
    return nullptr;
  }
  return socket;
}
