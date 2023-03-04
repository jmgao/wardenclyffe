#pragma once

#include <stdint.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <gui/SurfaceComposerClient.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/foundation/ALooper.h>
#include <ui/DisplayState.h>
#include <utils/StrongPointer.h>

#include "wardenclyffe/android/socket.h"
#include "wardenclyffe/wardenclyffe.h"

enum class FrameType { Description, Keyframe, Interframe };

struct Frame {
  std::vector<char> data;
  FrameType type;
  int64_t timestamp;
};

struct VideoSocket : public Socket {
  VideoSocket() = default;
  VideoSocket(bool emit_descriptors) : emit_descriptors_(emit_descriptors) {}

  virtual ~VideoSocket();

  WardenclyffeReads Read() final;

  static VideoSocket* Create(std::string_view path);

 protected:
  virtual bool Initialize() {
    return fetchDisplayParameters() && createEncoder() && createVirtualDisplay() &&
           prepareVirtualDisplay() && startEncoder();
  }

  bool fetchDisplayParameters();
  bool createVirtualDisplay();
  bool prepareVirtualDisplay();

  void setDisplayProjection(android::SurfaceComposerClient::Transaction& t,
                            android::sp<android::IBinder> display,
                            const android::ui::DisplayState& displayState);

  virtual bool createEncoder() = 0;
  virtual bool startEncoder() = 0;
  virtual void stopEncoder() {
    running_ = false;
    if (encoder_thread_) {
      encoder_thread_->join();
      encoder_thread_.reset();
    }
    cv_.notify_all();
  }

 protected:
  bool emit_descriptors_ = false;

  uint32_t video_width_ = 0;
  uint32_t video_height_ = 0;
  float video_framerate_ = 0;

  android::sp<android::IBinder> display_;
  android::sp<android::IGraphicBufferProducer> codec_surface_;

  android::ui::DisplayState display_state_;
  android::ui::DisplayMode display_mode_;

  std::atomic<bool> running_ = false;
  std::optional<std::thread> encoder_thread_;
  std::mutex mutex_;
  std::condition_variable cv_;

  std::deque<Frame> frames_ GUARDED_BY(mutex_);
  std::vector<char> partial_frame_ GUARDED_BY(mutex_);
  std::deque<std::string> descriptions_ GUARDED_BY(mutex_);
  std::vector<WardenclyffeRead> reads_ GUARDED_BY(mutex_);
};

struct H264Socket : public VideoSocket {
  H264Socket() = default;
  explicit H264Socket(bool emit_descriptors) : VideoSocket(emit_descriptors) {}

  ~H264Socket() {
    if (encoder_thread_) {
      stopEncoder();
    }

    if (codec_) {
      codec_->stop();
      codec_->release();
    }
  }

 protected:
  bool createEncoder() final;
  bool startEncoder() final;

 private:
  android::sp<android::ALooper> looper_;
  android::sp<android::MediaCodec> codec_;

  uint32_t video_bitrate_ = 20'000'000;
};
