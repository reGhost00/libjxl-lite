// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// JPEG XL backend of jxlimg, implemented with lib/jxl/.

#include "jxlimg_internal.h"

#include <cstring>

#include "jxl/color_encoding.h"
#include "jxl/decode.h"
#include "jxl/encode.h"
#include "jxl/memory_manager.h"

namespace jxlimg {
namespace {

void* JxlAlloc(void* opaque, size_t size) {
  (void)opaque;
  return Alloc(size);
}

void JxlFree(void* opaque, void* address) {
  (void)opaque;
  Free(address);
}

// The JPEG XL decoder and encoder use this allocator, so that all memory
// that jxlimg causes to be allocated comes from the user supplied
// allocator.
const JxlMemoryManager kMemoryManager = {nullptr, JxlAlloc, JxlFree};

constexpr size_t kEncodeChunkSize = 64 * 1024;

jxlimg_status JxlDecodeImpl(const uint8_t* data, size_t size, bool info_only,
                             uint8_t** pixels, int* width, int* height,
                             int* channels) {
  JxlDecoder* decoder = JxlDecoderCreate(&kMemoryManager);
  if (decoder == nullptr) {
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  const int events =
      info_only ? JXL_DEC_BASIC_INFO
                : static_cast<int>(JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
  if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(decoder, events)) {
    JxlDecoderDestroy(decoder);
    SetFailure("could not subscribe to the decoder events");
    return JXLLIMG_ERR_INTERNAL;
  }
  JxlDecoderSetInput(decoder, data, size);
  JxlDecoderCloseInput(decoder);

  JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  int image_channels = 0;
  uint8_t* buffer = nullptr;
  jxlimg_status status = JXLLIMG_OK;

  for (;;) {
    const JxlDecoderStatus decoder_status = JxlDecoderProcessInput(decoder);
    if (decoder_status == JXL_DEC_ERROR) {
      SetFailure("could not decode the JPEG XL image");
      status = JXLLIMG_ERR_DECODE_FAILED;
      break;
    }
    if (decoder_status == JXL_DEC_NEED_MORE_INPUT) {
      SetFailure("the JPEG XL image is truncated");
      status = JXLLIMG_ERR_DECODE_FAILED;
      break;
    }
    if (decoder_status == JXL_DEC_BASIC_INFO) {
      JxlBasicInfo info;
      if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(decoder, &info)) {
        SetFailure("could not read the JPEG XL image header");
        status = JXLLIMG_ERR_DECODE_FAILED;
        break;
      }
      image_width = info.xsize;
      image_height = info.ysize;
      // The natural channel count of the image: gray, gray+alpha, RGB or
      // RGBA.  The alpha channel of a JPEG XL image is an extra channel,
      // but the decoder is able to deliver it together with the color
      // channels.
      image_channels = info.num_color_channels + (info.alpha_bits > 0 ? 1 : 0);
      format.num_channels = static_cast<uint32_t>(image_channels);
      if (info_only) {
        JxlDecoderDestroy(decoder);
        *pixels = nullptr;
        *width = static_cast<int>(image_width);
        *height = static_cast<int>(image_height);
        *channels = image_channels;
        return JXLLIMG_OK;
      }
    } else if (decoder_status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t buffer_size = 0;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderImageOutBufferSize(decoder, &format, &buffer_size)) {
        SetFailure("could not determine the size of the JPEG XL image");
        status = JXLLIMG_ERR_DECODE_FAILED;
        break;
      }
      Free(buffer);
      buffer = reinterpret_cast<uint8_t*>(Alloc(buffer_size));
      if (buffer == nullptr) {
        SetFailure("out of memory");
        status = JXLLIMG_ERR_OUT_OF_MEMORY;
        break;
      }
      if (JXL_DEC_SUCCESS !=
          JxlDecoderSetImageOutBuffer(decoder, &format, buffer, buffer_size)) {
        SetFailure("could not decode the JPEG XL image");
        status = JXLLIMG_ERR_DECODE_FAILED;
        break;
      }
    } else if (decoder_status == JXL_DEC_FULL_IMAGE) {
      // Only the first frame is decoded, animations are not supported.
      break;
    } else if (decoder_status == JXL_DEC_SUCCESS) {
      break;
    }
  }

  JxlDecoderDestroy(decoder);
  if (status != JXLLIMG_OK) {
    Free(buffer);
    return status;
  }
  if (buffer == nullptr || image_width == 0 || image_height == 0) {
    Free(buffer);
    SetFailure("the JPEG XL image has no pixels");
    return JXLLIMG_ERR_DECODE_FAILED;
  }
  *pixels = buffer;
  *width = static_cast<int>(image_width);
  *height = static_cast<int>(image_height);
  *channels = image_channels;
  return JXLLIMG_OK;
}

jxlimg_status JxlEncodeImpl(const uint8_t* pixels, int width, int height,
                             int channels, int quality,
                             const jxlimg_options& options,
                             uint8_t** out_data, size_t* out_size) {
  JxlEncoder* encoder = JxlEncoderCreate(&kMemoryManager);
  if (encoder == nullptr) {
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  const bool has_alpha = (channels == 4);
  const int color_channels = has_alpha ? 3 : channels;
  jxlimg_status status = JXLLIMG_OK;

  JxlEncoderUseContainer(encoder, options.jxl_container ? JXL_TRUE : JXL_FALSE);

  JxlBasicInfo info;
  JxlEncoderInitBasicInfo(&info);
  info.xsize = static_cast<uint32_t>(width);
  info.ysize = static_cast<uint32_t>(height);
  info.bits_per_sample = 8;
  info.exponent_bits_per_sample = 0;
  info.num_color_channels = static_cast<uint32_t>(color_channels);
  info.num_extra_channels = has_alpha ? 1 : 0;
  info.alpha_bits = has_alpha ? 8 : 0;
  float distance = JxlEncoderDistanceFromQuality(static_cast<float>(quality));
  // A distance of zero means that the encoder switches to lossless mode,
  // which is only possible without the perceptual XYB transform.
  const bool lossless = options.jxl_lossless || distance <= 0.f;
  if (lossless) {
    info.uses_original_profile = JXL_TRUE;
    distance = 0.f;
  }
  if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(encoder, &info)) {
    JxlEncoderDestroy(encoder);
    SetFailure("could not set the JPEG XL image header");
    return JXLLIMG_ERR_ENCODE_FAILED;
  }
  JxlColorEncoding color_encoding;
  JxlColorEncodingSetToSRGB(&color_encoding,
                             color_channels == 1 ? JXL_TRUE : JXL_FALSE);
  if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(encoder, &color_encoding)) {
    JxlEncoderDestroy(encoder);
    SetFailure("could not set the color encoding");
    return JXLLIMG_ERR_ENCODE_FAILED;
  }

  JxlEncoderFrameSettings* frame_settings =
      JxlEncoderFrameSettingsCreate(encoder, nullptr);
  if (frame_settings == nullptr) {
    JxlEncoderDestroy(encoder);
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  if (JXL_ENC_SUCCESS !=
      JxlEncoderSetFrameDistance(frame_settings, distance)) {
    JxlEncoderDestroy(encoder);
    SetFailure("could not set the JPEG XL quality");
    return JXLLIMG_ERR_ENCODE_FAILED;
  }
  if (JXL_ENC_SUCCESS !=
      JxlEncoderFrameSettingsSetOption(frame_settings,
                                       JXL_ENC_FRAME_SETTING_EFFORT,
                                       static_cast<int32_t>(options.jxl_effort))) {
    JxlEncoderDestroy(encoder);
    SetFailure("could not set the JPEG XL effort");
    return JXLLIMG_ERR_ENCODE_FAILED;
  }

  // The JPEG XL encoder takes the alpha channel interleaved with the color
  // channels, so the pixel data can be passed on unchanged.
  const size_t num_pixels = static_cast<size_t>(width) * height;
  JxlPixelFormat pixel_format = {static_cast<uint32_t>(channels),
                                 JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  const size_t pixel_size = num_pixels * channels;
  if (JXL_ENC_SUCCESS !=
      JxlEncoderAddImageFrame(frame_settings, &pixel_format, pixels,
                              pixel_size)) {
    JxlEncoderDestroy(encoder);
    SetFailure("could not add the image to the JPEG XL encoder");
    return JXLLIMG_ERR_ENCODE_FAILED;
  }
  JxlEncoderCloseInput(encoder);

  OutputBuffer buffer;
  uint8_t chunk[kEncodeChunkSize];
  for (;;) {
    uint8_t* next_out = chunk;
    size_t avail_out = sizeof(chunk);
    const JxlEncoderStatus encoder_status =
        JxlEncoderProcessOutput(encoder, &next_out, &avail_out);
    const size_t written = sizeof(chunk) - avail_out;
    if (written > 0 && !buffer.Append(chunk, written)) {
      SetFailure("out of memory");
      status = JXLLIMG_ERR_OUT_OF_MEMORY;
      break;
    }
    if (encoder_status == JXL_ENC_SUCCESS) break;
    if (encoder_status != JXL_ENC_NEED_MORE_OUTPUT) {
      SetFailure("could not encode the JPEG XL image");
      status = JXLLIMG_ERR_ENCODE_FAILED;
      break;
    }
  }

  JxlEncoderDestroy(encoder);
  if (status != JXLLIMG_OK) return status;
  *out_size = buffer.size();
  *out_data = buffer.Release();
  return JXLLIMG_OK;
}

}  // namespace

jxlimg_status DecodeJxl(const uint8_t* data, size_t size, bool info_only,
                        Image* out) {
  uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int channels = 0;
  const jxlimg_status status =
      JxlDecodeImpl(data, size, info_only, &pixels, &width, &height, &channels);
  if (status != JXLLIMG_OK) return status;
  out->pixels = pixels;
  out->width = width;
  out->height = height;
  out->channels = channels;
  return JXLLIMG_OK;
}

jxlimg_status EncodeJxl(const uint8_t* pixels, int width, int height,
                        int channels, int quality,
                        const jxlimg_options& options, OutputBuffer* out) {
  uint8_t* data = nullptr;
  size_t size = 0;
  const jxlimg_status status = JxlEncodeImpl(pixels, width, height, channels,
                                              quality, options, &data, &size);
  if (status != JXLLIMG_OK) return status;
  const bool ok = out->Append(data, size);
  Free(data);
  if (!ok) {
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  return JXLLIMG_OK;
}

}  // namespace jxlimg
