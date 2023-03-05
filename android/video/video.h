#pragma once

#include <stdint.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <gui/BufferItem.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>
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
  virtual ~VideoSocket() { VideoSocket::Destroy(); }

  static VideoSocket* Create(std::string_view path);

  virtual WardenclyffeReads Read() final;

  bool Initialize() EXCLUDES(buffer_queue_mutex_) {
    std::lock_guard<std::mutex> lock(buffer_queue_mutex_);
    return fetchDisplayParameters() && createEncoder() && createVirtualDisplay() &&
           prepareVirtualDisplay() && startEncoder();
  }

  virtual void Destroy() override EXCLUDES(buffer_queue_mutex_) {
    std::lock_guard<std::mutex> lock(buffer_queue_mutex_);
    DestroyLocked();
  }

  virtual void DestroyLocked() REQUIRES(buffer_queue_mutex_) {
    destroyVirtualDisplay();
  }

 protected:
  bool fetchDisplayParameters();
  bool createVirtualDisplay() REQUIRES(buffer_queue_mutex_);
  bool prepareVirtualDisplay() REQUIRES(buffer_queue_mutex_);

  void destroyVirtualDisplay() REQUIRES(buffer_queue_mutex_);

  void setDisplayProjection(android::SurfaceComposerClient::Transaction& t,
                            android::sp<android::IBinder> display,
                            const android::ui::DisplayState& display_state);

  virtual void onFrameReceived() = 0;

  virtual bool createEncoder() REQUIRES(buffer_queue_mutex_) = 0;
  virtual bool startEncoder() REQUIRES(buffer_queue_mutex_) = 0;
  virtual uint64_t getGrallocUsageBits() = 0;

  bool emit_descriptors_ = false;

  uint32_t video_width_ = 0;
  uint32_t video_height_ = 0;
  float video_framerate_ = 0;

  std::mutex buffer_queue_mutex_;
  android::sp<android::IBinder> display_;
  android::sp<android::IGraphicBufferConsumer> display_consumer_ GUARDED_BY(buffer_queue_mutex_);
  android::sp<android::IGraphicBufferProducer> display_producer_ GUARDED_BY(buffer_queue_mutex_);

  android::ui::DisplayState display_state_;
  android::ui::DisplayMode display_mode_;

  std::mutex frame_mutex_ ACQUIRED_BEFORE(buffer_queue_mutex_);
  std::condition_variable cv_;
  std::atomic<bool> running_ = false;

  std::deque<Frame> frames_ GUARDED_BY(frame_mutex_);
  std::vector<char> partial_frame_ GUARDED_BY(frame_mutex_);
  std::deque<std::string> descriptions_ GUARDED_BY(frame_mutex_);
  std::vector<WardenclyffeRead> reads_ GUARDED_BY(frame_mutex_);

  struct DisplayBufferConsumerCallbacks : public android::BnConsumerListener {
    explicit DisplayBufferConsumerCallbacks(VideoSocket& parent) : parent_(parent) {}

    virtual void onDisconnect() final {}
    virtual void onFrameDequeued(const uint64_t) final {}
    virtual void onFrameCancelled(const uint64_t) final {}
    virtual void onFrameDetached(const uint64_t) final {}
    virtual void onFrameAvailable(const android::BufferItem& item) final;
    virtual void onFrameReplaced(const android::BufferItem& item) final;
    virtual void onBuffersReleased() final;
    virtual void onSidebandStreamChanged() final {}

   private:
    VideoSocket& parent_;
  };

  struct DisplayBufferProducerCallbacks : public android::BnProducerListener {
    explicit DisplayBufferProducerCallbacks(VideoSocket& parent) : parent_(parent) {}

    virtual void onBufferReleased() final;
    virtual bool needsReleaseNotify() final;
    virtual void onBuffersDiscarded(const std::vector<int32_t>& slots) final;

   private:
    [[maybe_unused]] VideoSocket& parent_;
  };
};

struct MediaCodecSocket : public VideoSocket {
  MediaCodecSocket() = default;
  ~MediaCodecSocket() {
    std::lock_guard<std::mutex> lock(buffer_queue_mutex_);
    MediaCodecSocket::DestroyLocked();
  }

  virtual void DestroyLocked() override REQUIRES(buffer_queue_mutex_) {
    stopEncoder();
    VideoSocket::DestroyLocked();
    destroyEncoder();
  }

 protected:
  virtual const char* getCodecMimeType() = 0;
  virtual android::sp<android::AMessage> getCodecFormat() = 0;

  virtual uint64_t getGrallocUsageBits() final {
    return GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_VIDEO_ENCODER;
  }

  virtual void onFrameReceived() final;

  virtual bool createEncoder() final REQUIRES(buffer_queue_mutex_);
  virtual bool startEncoder() final REQUIRES(buffer_queue_mutex_);
  void stopEncoder() REQUIRES(buffer_queue_mutex_);
  void destroyEncoder() REQUIRES(buffer_queue_mutex_);

  std::optional<std::thread> encoder_thread_;

  android::sp<android::ALooper> looper_;
  android::sp<android::MediaCodec> codec_ GUARDED_BY(buffer_queue_mutex_);
  android::sp<android::IGraphicBufferProducer> codec_producer_ GUARDED_BY(buffer_queue_mutex_);

  struct CodecBufferProducerCallbacks : public android::BnProducerListener {
    explicit CodecBufferProducerCallbacks(MediaCodecSocket& parent) : parent_(parent) {}

    virtual void onBufferReleased() final;
    virtual bool needsReleaseNotify() final;
    virtual void onBuffersDiscarded(const std::vector<int32_t>& slots) final;

   private:
    [[maybe_unused]] MediaCodecSocket& parent_;
  };
};

struct H264Socket : public MediaCodecSocket {
  H264Socket() = default;

 protected:
  virtual const char* getCodecMimeType() final;
  virtual android::sp<android::AMessage> getCodecFormat() final;

  uint32_t video_bitrate_ = 20'000'000;
};

struct JPEGSocket : public VideoSocket {
  JPEGSocket() = default;

 protected:
  virtual void onFrameReceived() final;

  virtual uint64_t getGrallocUsageBits() final {
    return GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_VIDEO_ENCODER;
  }

  virtual bool createEncoder() final { return true; }
  virtual bool startEncoder() final {
    running_ = true;
    return true;
  }
};
