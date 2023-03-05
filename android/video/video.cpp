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

// libbase has CHECK macros that conflict with stagefright's ADebug.h
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <android-base/logging.h>
#pragma clang diagnostic pop

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

VideoSocket* VideoSocket::Create(std::string_view path) {
  VideoSocket* result = nullptr;
  if (android::base::ConsumePrefix(&path, "h264/")) {
    result = new H264Socket();
  } else if (android::base::ConsumePrefix(&path, "jpeg/")) {
    result = new JPEGSocket();
  }

  if (result && !result->Initialize()) {
    delete result;
    result = nullptr;
  }

  return result;
}

WardenclyffeReads VideoSocket::Read() {
  std::unique_lock<std::mutex> lock(frame_mutex_);
  base::ScopedLockAssertion lock_assertion(frame_mutex_);
  cv_.wait(lock, [this]() {
    base::ScopedLockAssertion lock_assertion(frame_mutex_);
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

void VideoSocket::DisplayBufferConsumerCallbacks::onFrameAvailable(const BufferItem&) {
  parent_.onFrameReceived();
}

void VideoSocket::DisplayBufferConsumerCallbacks::onFrameReplaced(const BufferItem&) {
  parent_.onFrameReceived();
}

void VideoSocket::DisplayBufferConsumerCallbacks::onBuffersReleased() {
  LOG(INFO) << "DisplayBufferConsumer: onBuffersReleased";
}

void VideoSocket::DisplayBufferProducerCallbacks::onBufferReleased() {
  LOG(INFO) << "DisplayBufferProducer: onBufferReleased";
}

bool VideoSocket::DisplayBufferProducerCallbacks::needsReleaseNotify() {
  return true;
}

void VideoSocket::DisplayBufferProducerCallbacks::onBuffersDiscarded(const std::vector<int32_t>&) {
  LOG(INFO) << "DisplayBufferProducer: onBuffersDiscarded";
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
                                       const ui::DisplayState& display_state) {
  // Set the region of the layer stack we're interested in, which in our case is "all of it".
  Rect layer_stack_rect(display_state.layerStackSpaceRect);

  // We need to preserve the aspect ratio of the display.
  float display_aspect =
      layer_stack_rect.getHeight() / static_cast<float>(layer_stack_rect.getWidth());

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
  uint32_t video_width = video_width_;
  uint32_t video_height = video_height_;
  uint32_t out_width, out_height;

  if (video_height > static_cast<uint32_t>(video_width * display_aspect)) {
    // limited by narrow width; reduce height
    out_width = video_width;
    out_height = static_cast<uint32_t>(video_width * display_aspect);
  } else {
    // limited by short height; restrict width
    out_height = video_height;
    out_width = static_cast<uint32_t>(video_height / display_aspect);
  }
  uint32_t off_x, off_y;
  off_x = (video_width - out_width) / 2;
  off_y = (video_height - out_height) / 2;
  Rect display_rect(off_x, off_y, off_x + out_width, off_y + out_height);

  t.setDisplayProjection(display, ui::ROTATION_0, layer_stack_rect, display_rect);
}

bool VideoSocket::createVirtualDisplay() {
  display_ = SurfaceComposerClient::createDisplay(String8("wardenclyffe"), false /*secure*/);
  if (!display_) {
    LOG(ERROR) << "failed to create virtual display";
    return false;
  }

  auto display_consumer_callbacks = sp<DisplayBufferConsumerCallbacks>::make(*this);
  auto display_producer_callbacks = sp<DisplayBufferProducerCallbacks>::make(*this);
  BnGraphicBufferProducer::QueueBufferOutput queue_buffer_output;

  BufferQueue::createBufferQueue(&display_producer_, &display_consumer_);
  display_consumer_->setDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888);
  display_consumer_->setDefaultBufferSize(video_width_, video_height_);

  display_consumer_->setConsumerUsageBits(getGrallocUsageBits());
  display_consumer_->consumerConnect(display_consumer_callbacks, true);
  display_producer_->connect(display_producer_callbacks, NATIVE_WINDOW_API_MEDIA, true,
                             &queue_buffer_output);
  LOG(INFO) << "connected to display BufferQueue";

  SurfaceComposerClient::Transaction t;
  t.setDisplaySurface(display_, display_producer_);
  t.apply();

  return true;
}

bool VideoSocket::prepareVirtualDisplay() {
  SurfaceComposerClient::Transaction t;
  setDisplayProjection(t, display_, display_state_);
  t.setDisplayLayerStack(display_, display_state_.layerStack);
  t.apply();
  return true;
}

void VideoSocket::destroyVirtualDisplay() {
  LOG(INFO) << "destroying virtual display";
  if (display_) {
    SurfaceComposerClient::destroyDisplay(display_);
    display_ = nullptr;
  }
  if (display_consumer_) {
    display_consumer_->consumerDisconnect();
    display_consumer_ = nullptr;
  }
  if (display_producer_) {
    display_producer_->disconnect(NATIVE_WINDOW_API_MEDIA);
    display_producer_ = nullptr;
  }
}

void MediaCodecSocket::CodecBufferProducerCallbacks::onBufferReleased() {
  // TODO: Pass the buffer back over to the virtual display.
}

bool MediaCodecSocket::CodecBufferProducerCallbacks::needsReleaseNotify() {
  return false;
}

void MediaCodecSocket::CodecBufferProducerCallbacks::onBuffersDiscarded(
    const std::vector<int32_t>&) {
  LOG(INFO) << "CodecBufferProduce: onBuffersDiscarded";
}

void MediaCodecSocket::onFrameReceived() {
  std::lock_guard<std::mutex> lock1(frame_mutex_);
  std::lock_guard<std::mutex> lock2(buffer_queue_mutex_);

  if (!display_consumer_) {
    LOG(INFO) << "display consumer was destroyed";
    return;
  }

  BufferItem item;
  status_t rc = display_consumer_->acquireBuffer(&item, 0);
  if (rc != NO_ERROR) {
    LOG(FATAL) << "failed to acquire buffer from IGraphicBufferConsumer: " << statusToString(rc);
  }

  rc = display_consumer_->detachBuffer(item.mSlot);
  if (rc != NO_ERROR) {
    LOG(FATAL) << "failed to detach buffer from IGraphicBufferConsumer: " << statusToString(rc);
  }

  int codec_slot;
  rc = codec_producer_->attachBuffer(&codec_slot, item.mGraphicBuffer);
  if (rc != NO_ERROR) {
    LOG(FATAL) << "failed to attach buffer to IGraphicBufferProducer: " << statusToString(rc);
  }

  BnGraphicBufferProducer::QueueBufferInput queue_buffer_input(
      item.mTimestamp, item.mIsAutoTimestamp, item.mDataSpace, item.mCrop, item.mScalingMode,
      item.mTransform, item.mFence);
  BnGraphicBufferProducer::QueueBufferOutput output;
  rc = codec_producer_->queueBuffer(codec_slot, queue_buffer_input, &output);
  if (rc != NO_ERROR) {
    LOG(FATAL) << "failed to queue buffer to IGraphicBufferProducer: " << statusToString(rc);
  }
}

bool MediaCodecSocket::createEncoder() {
  looper_ = new ALooper();
  looper_->setName("wardenclyffe_looper");
  looper_->start();

  codec_ = MediaCodec::CreateByType(looper_, getCodecMimeType(), true);
  if (!codec_) {
    LOG(ERROR) << "Failed to create codec instance";
    return false;
  }

  sp<AMessage> format = getCodecFormat();
  status_t err = codec_->configure(format, nullptr, nullptr, MediaCodec::CONFIGURE_FLAG_ENCODE);
  if (err != NO_ERROR) {
    LOG(ERROR) << "Failed to configure codec at " << video_width_ << "x" << video_height_
               << " (err = " << err << ")";
    return false;
  }

  err = codec_->createInputSurface(&codec_producer_);
  if (err != NO_ERROR) {
    LOG(ERROR) << "Failed to create encoder input surface (err = " << err << ")";
    return false;
  }

  BnGraphicBufferProducer::QueueBufferOutput queue_buffer_output;
  auto codec_producer_callbacks = sp<CodecBufferProducerCallbacks>::make(*this);
  err = codec_producer_->connect(codec_producer_callbacks, NATIVE_WINDOW_API_MEDIA, true,
                                 &queue_buffer_output);
  if (err != NO_ERROR) {
    LOG(ERROR) << "Failed to connect to encoder input surface";
    return false;
  }

  err = codec_->start();
  if (err != NO_ERROR) {
    LOG(ERROR) << "Failed to start codec (err = " << err << ")";
    return false;
  }

  LOG(INFO) << "Codec instantiated";
  return true;
}

bool MediaCodecSocket::startEncoder() {
  CHECK(!running_);
  running_ = true;
  encoder_thread_ = std::thread([this]() {
    android::sp<android::MediaCodec> codec;
    Vector<sp<MediaCodecBuffer>> buffers;
    status_t err;

    {
      std::lock_guard<std::mutex> lock(buffer_queue_mutex_);
      codec = codec_;
    }

    err = codec->getOutputBuffers(&buffers);
    if (err != NO_ERROR) {
      LOG(FATAL) << "failed to get output buffers (err = " << err << ")";
    }

    bool skip = false;
    while (running_) {
      size_t buf_index, offset, size;
      int64_t pts_usec;
      uint32_t flags;

      static constexpr int kTimeout = 250000;
      err = codec->dequeueOutputBuffer(&buf_index, &offset, &size, &pts_usec, &flags, kTimeout);
      switch (err) {
        case NO_ERROR:
          if (size != 0) {
            LOG(VERBOSE) << "Got data in buffer " << buf_index << ", size = " << size
                         << ", pts = " << pts_usec;

            // Check orientation, update if it has changed.
            //
            // Polling for changes is inefficient and wrong, but the
            // useful stuff is hard to get at without a Dalvik VM.
            ui::DisplayState current_display_state;
            err = SurfaceComposerClient::getDisplayState(display_, &current_display_state);
            if (err != NO_ERROR) {
              LOG(WARNING) << "getDisplayState failed (err = " << err << ")";
            } else if (display_state_.orientation != current_display_state.orientation ||
                       display_state_.layerStack != current_display_state.layerStack) {
              LOG(INFO) << "Updating display state";
              std::lock_guard<std::mutex> lock(buffer_queue_mutex_);
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

            std::lock_guard<std::mutex> lock(frame_mutex_);
            char* p = reinterpret_cast<char*>(buffers[buf_index]->data());
            if (frame.type == FrameType::Description) {
              partial_frame_.assign(p, p + size);
            } else {
              std::vector<char> data = std::move(partial_frame_);

              if (!skip) {
                data.insert(data.end(), p, p + size);
                frame.data = std::move(data);
                frame.timestamp = pts_usec;
                frames_.push_back(std::move(frame));
                cv_.notify_one();
              }
              skip = !skip;
            }
          }
          err = codec->releaseOutputBuffer(buf_index);

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
          err = codec->getOutputBuffers(&buffers);
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

void MediaCodecSocket::stopEncoder() REQUIRES(buffer_queue_mutex_) {
  running_ = false;
  buffer_queue_mutex_.unlock();
  cv_.notify_all();
  if (encoder_thread_) {
    encoder_thread_->join();
    encoder_thread_.reset();
  }
  buffer_queue_mutex_.lock();
}

void MediaCodecSocket::destroyEncoder() {
  if (codec_producer_) {
    codec_producer_->disconnect(NATIVE_WINDOW_API_MEDIA);
    codec_producer_ = nullptr;
  }

  if (codec_) {
    codec_->stop();
    codec_->release();
    codec_ = nullptr;
  }
}
