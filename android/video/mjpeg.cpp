#include <android-base/logging.h>
#include <android/bitmap.h>
#include <binder/IPCThreadState.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <utils/StrongPointer.h>

#include "wardenclyffe/android/video/video.h"

using namespace android;

void JPEGSocket::onFrameReceived() {
  status_t rc;
  BufferItem item;

  {
    std::lock_guard<std::mutex> lock1(frame_mutex_);
    std::lock_guard<std::mutex> lock2(buffer_queue_mutex_);

    if (!display_consumer_) {
      LOG(INFO) << "display consumer was destroyed";
      return;
    }

    rc = display_consumer_->acquireBuffer(&item, 0);
    if (rc != NO_ERROR) {
      LOG(FATAL) << "failed to acquire buffer from IGraphicBufferConsumer: " << statusToString(rc);
    }

    // TODO: Hand this buffer back to the virtual display.
    rc = display_consumer_->detachBuffer(item.mSlot);
    if (rc != NO_ERROR) {
      LOG(FATAL) << "failed to detach buffer from IGraphicBufferConsumer: " << statusToString(rc);
    }
  }

  sp<GraphicBuffer> buffer = item.mGraphicBuffer;
  void* base = nullptr;
  rc = buffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, &base);
  if (rc != NO_ERROR) {
    LOG(FATAL) << "failed to lock GraphicBuffer: " << statusToString(rc);
  }

  AndroidBitmapInfo info;
  info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
  info.flags = ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;
  info.width = buffer->getWidth();
  info.height = buffer->getHeight();
  info.stride = buffer->getStride() * bytesPerPixel(buffer->getPixelFormat());

  std::vector<char> buf;
  int result =
      AndroidBitmap_compress(&info, ADATASPACE_SRGB, base, ANDROID_BITMAP_COMPRESS_FORMAT_JPEG, 90,
                             &buf, [](void* userdata, const void* data, size_t size) -> bool {
                               auto buf = static_cast<std::vector<char>*>(userdata);
                               auto p = static_cast<const char*>(data);
                               buf->insert(buf->end(), p, p + size);
                               return true;
                             });

  if (result != ANDROID_BITMAP_RESULT_SUCCESS) {
    LOG(FATAL) << "AndroidBitmap_compress failed (rc = " << result << ")";
  }

  rc = buffer->unlock();
  if (rc != NO_ERROR) {
    LOG(FATAL) << "failed to unlock GraphicBuffer: " << statusToString(rc);
  }

  Frame frame;
  frame.type = FrameType::Keyframe;

  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    frame.data = std::move(buf);
    frame.timestamp = item.mTimestamp;
    frames_.push_back(std::move(frame));
  }
  cv_.notify_one();
}
