#include <binder/IPCThreadState.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <utils/StrongPointer.h>

#include "wardenclyffe/android/video/video.h"

using namespace android;

const char* H264Socket::getCodecMimeType() {
  return "video/avc";
}

sp<AMessage> H264Socket::getCodecFormat() {
  auto format = sp<AMessage>::make();
  format->setInt32(KEY_WIDTH, video_width_);
  format->setInt32(KEY_HEIGHT, video_height_);
  format->setString(KEY_MIME, getCodecMimeType());
  format->setInt32(KEY_COLOR_FORMAT, PIXEL_FORMAT_RGBA_8888);

  format->setInt32(KEY_BITRATE_MODE, BITRATE_MODE_CBR);
  format->setInt32(KEY_BIT_RATE, 10'000'000);

  format->setFloat(KEY_FRAME_RATE, video_framerate_);
  format->setFloat(KEY_MAX_FPS_TO_ENCODER, video_framerate_);
  format->setInt32(KEY_REPEAT_PREVIOUS_FRAME_AFTER, 1'000'000 / video_framerate_);

  format->setInt32(KEY_I_FRAME_INTERVAL, -1);
  format->setInt32(KEY_MAX_B_FRAMES, 0);
  format->setInt32(KEY_PROFILE, AVCProfileMain);
  format->setInt32(KEY_LEVEL, AVCLevel41);
  format->setInt32(KEY_PRIORITY, 0);
  format->setInt32(KEY_LOW_LATENCY, 1);
  return format;
}
