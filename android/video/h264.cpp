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

#include "wardenclyffe/android/socket.h"
#include "wardenclyffe/android/video/video.h"

using namespace android;

static constexpr char kMimeTypeAvc[] = "video/avc";

bool H264Socket::createEncoder() {
  sp<AMessage> format = new AMessage();
  format->setInt32(KEY_WIDTH, video_width_);
  format->setInt32(KEY_HEIGHT, video_height_);
  format->setString(KEY_MIME, kMimeTypeAvc);
  format->setInt32(KEY_COLOR_FORMAT, OMX_COLOR_FormatAndroidOpaque);
  format->setInt32(KEY_BIT_RATE, video_bitrate_);
  format->setFloat(KEY_FRAME_RATE, video_framerate_);
  format->setInt32(KEY_I_FRAME_INTERVAL, 0);
  format->setInt32(KEY_MAX_B_FRAMES, 0);
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

  err = codec_->createInputSurface(&codec_surface_);
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

bool H264Socket::startEncoder() {
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
