// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// JPEG backend of jxlimg, implemented with the in-tree lib/jpegli/ codec.
//
// Two things need special care here:
//
//  * lib/jpegli/ calls exit() when it runs into an error.  A library must
//    not do that, so the error handler is replaced by one that longjmps back
//    into the caller (see JpegErrorExit below).
//  * longjmp() invalidates the value of local variables that were modified
//    between setjmp() and longjmp(), and it skips C++ destructors.  All
//    state that has to survive an error is therefore kept in objects that
//    are allocated on the heap, and the output values are passed as
//    pointers to the caller.

#include "jxlimg_internal.h"

#include <csetjmp>
#include <cstdio>
#include <cstring>

#include "lib/jpegli/decode.h"
#include "lib/jpegli/encode.h"

namespace jxlimg {
namespace {

constexpr size_t kJpegMessageSize = 256;
constexpr size_t kJpegDestinationCapacity = 64 * 1024;

// Error manager of libjpeg.  The layout has to start with jpeg_error_mgr,
// because libjpeg casts between the two.
struct JpegErrorManager {
  struct jpeg_error_mgr pub;
  jmp_buf jump_buffer;
  char message[kJpegMessageSize];
};

// Called by lib/jpegli/ when it encounters an error.  The default handler
// would call exit(), which is not acceptable inside a library.
void JpegErrorExit(j_common_ptr cinfo) {
  auto* error = reinterpret_cast<JpegErrorManager*>(cinfo->err);
  // lib/jpegli/ stores the error text in msg_parm.s, see
  // lib/jpegli/error.cc.
  snprintf(error->message, sizeof(error->message), "%s",
           cinfo->err->msg_parm.s);
  longjmp(error->jump_buffer, 1);
}

// Replaces the default handler that prints warnings to stderr.
void JpegSilentMessage(j_common_ptr cinfo) { (void)cinfo; }

// Growable destination manager that uses the allocator of jxlimg.  It is
// allocated on the heap, so that it survives a longjmp().
struct JpegDestinationManager {
  struct jpeg_destination_mgr pub;
  uint8_t* buffer;
  size_t capacity;
  size_t written;
};

void JpegDestinationInit(j_compress_ptr cinfo) {
  auto* dest = reinterpret_cast<JpegDestinationManager*>(cinfo->dest);
  dest->pub.next_output_byte = dest->buffer;
  dest->pub.free_in_buffer = dest->capacity;
}

boolean JpegDestinationEmpty(j_compress_ptr cinfo) {
  auto* dest = reinterpret_cast<JpegDestinationManager*>(cinfo->dest);
  const size_t used = dest->capacity - dest->pub.free_in_buffer;
  const size_t new_capacity = dest->capacity * 2;
  uint8_t* new_buffer = reinterpret_cast<uint8_t*>(Alloc(new_capacity));
  if (new_buffer == nullptr) return FALSE;
  memcpy(new_buffer, dest->buffer, used);
  Free(dest->buffer);
  dest->buffer = new_buffer;
  dest->capacity = new_capacity;
  dest->pub.next_output_byte = new_buffer + used;
  dest->pub.free_in_buffer = new_capacity - used;
  return TRUE;
}

void JpegDestinationTerm(j_compress_ptr cinfo) {
  auto* dest = reinterpret_cast<JpegDestinationManager*>(cinfo->dest);
  dest->written = dest->capacity - dest->pub.free_in_buffer;
}

// Number of components of the source image that JPEG can store.
int JpegInputComponents(int channels) { return channels == 1 ? 1 : 3; }

jxlimg_status JpegDecodeImpl(const uint8_t* data, size_t size, bool info_only,
                              uint8_t** pixels, int* width, int* height,
                              int* channels) {
  // jpegli_CreateDecompress() does not initialize the whole structure, so it
  // has to be zeroed here.  It is allocated on the heap so that it stays
  // valid after a longjmp().
  auto* cinfo = reinterpret_cast<struct jpeg_decompress_struct*>(
      Alloc(sizeof(struct jpeg_decompress_struct)));
  if (cinfo == nullptr) {
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  memset(cinfo, 0, sizeof(*cinfo));

  JpegErrorManager error;
  cinfo->err = jpegli_std_error(&error.pub);
  cinfo->err->error_exit = JpegErrorExit;
  cinfo->err->output_message = JpegSilentMessage;

  if (setjmp(error.jump_buffer) != 0) {
    jpegli_destroy_decompress(cinfo);
    Free(cinfo);
    SetFailure("%s", error.message);
    return JXLLIMG_ERR_DECODE_FAILED;
  }

  jpegli_create_decompress(cinfo);
  jpegli_mem_src(cinfo, data, static_cast<unsigned long>(size));
  if (jpegli_read_header(cinfo, TRUE) != JPEG_HEADER_OK) {
    jpegli_destroy_decompress(cinfo);
    Free(cinfo);
    SetFailure("not a valid JPEG image");
    return JXLLIMG_ERR_DECODE_FAILED;
  }
  if (info_only) {
    // Grayscale images stay grayscale, everything else is converted to RGB.
    const int info_channels =
        (cinfo->jpeg_color_space == JCS_GRAYSCALE) ? 1 : 3;
    *width = static_cast<int>(cinfo->image_width);
    *height = static_cast<int>(cinfo->image_height);
    *channels = info_channels;
    jpegli_destroy_decompress(cinfo);
    Free(cinfo);
    *pixels = nullptr;
    return JXLLIMG_OK;
  }
  // Grayscale images stay grayscale, everything else is converted to RGB.
  cinfo->out_color_space =
      (cinfo->jpeg_color_space == JCS_GRAYSCALE) ? JCS_GRAYSCALE : JCS_RGB;
  jpegli_start_decompress(cinfo);

  const int out_width = static_cast<int>(cinfo->output_width);
  const int out_height = static_cast<int>(cinfo->output_height);
  const int out_channels = cinfo->output_components;
  const size_t row_size = static_cast<size_t>(out_width) * out_channels;
  uint8_t* buffer = reinterpret_cast<uint8_t*>(Alloc(row_size * out_height));
  if (buffer == nullptr) {
    jpegli_destroy_decompress(cinfo);
    Free(cinfo);
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  while (cinfo->output_scanline < cinfo->output_height) {
    JSAMPROW row =
        buffer + static_cast<size_t>(cinfo->output_scanline) * row_size;
    jpegli_read_scanlines(cinfo, &row, 1);
  }
  jpegli_finish_decompress(cinfo);
  jpegli_destroy_decompress(cinfo);
  Free(cinfo);

  *pixels = buffer;
  *width = out_width;
  *height = out_height;
  *channels = out_channels;
  return JXLLIMG_OK;
}

jxlimg_status JpegEncodeImpl(const uint8_t* pixels, int width, int height,
                              int channels, int quality,
                              const jxlimg_options& options,
                              uint8_t** out_data, size_t* out_size) {
  const int input_components = JpegInputComponents(channels);
  // JPEG has no alpha channel, drop it.
  uint8_t* rgb_pixels = nullptr;
  if (channels == 4) {
    const size_t num_pixels = static_cast<size_t>(width) * height;
    rgb_pixels = reinterpret_cast<uint8_t*>(Alloc(num_pixels * 3));
    if (rgb_pixels == nullptr) {
      SetFailure("out of memory");
      return JXLLIMG_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < num_pixels; ++i) {
      rgb_pixels[i * 3 + 0] = pixels[i * 4 + 0];
      rgb_pixels[i * 3 + 1] = pixels[i * 4 + 1];
      rgb_pixels[i * 3 + 2] = pixels[i * 4 + 2];
    }
    pixels = rgb_pixels;
  }

  auto* cinfo = reinterpret_cast<struct jpeg_compress_struct*>(
      Alloc(sizeof(struct jpeg_compress_struct)));
  auto* dest = reinterpret_cast<JpegDestinationManager*>(
      Alloc(sizeof(JpegDestinationManager)));
  if (cinfo == nullptr || dest == nullptr) {
    Free(cinfo);
    Free(dest);
    Free(rgb_pixels);
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  memset(cinfo, 0, sizeof(*cinfo));
  memset(dest, 0, sizeof(*dest));

  JpegErrorManager error;
  cinfo->err = jpegli_std_error(&error.pub);
  cinfo->err->error_exit = JpegErrorExit;
  cinfo->err->output_message = JpegSilentMessage;

  dest->buffer = reinterpret_cast<uint8_t*>(Alloc(kJpegDestinationCapacity));
  if (dest->buffer == nullptr) {
    Free(cinfo);
    Free(dest);
    Free(rgb_pixels);
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  dest->capacity = kJpegDestinationCapacity;
  dest->written = 0;
  dest->pub.init_destination = JpegDestinationInit;
  dest->pub.empty_output_buffer = JpegDestinationEmpty;
  dest->pub.term_destination = JpegDestinationTerm;

  if (setjmp(error.jump_buffer) != 0) {
    jpegli_destroy_compress(cinfo);
    Free(dest->buffer);
    Free(dest);
    Free(cinfo);
    Free(rgb_pixels);
    SetFailure("%s", error.message);
    return JXLLIMG_ERR_ENCODE_FAILED;
  }

  jpegli_create_compress(cinfo);
  // jpegli_create_compress() resets the destination manager, so it has to be
  // installed afterwards.
  cinfo->dest = &dest->pub;
  cinfo->image_width = static_cast<JDIMENSION>(width);
  cinfo->image_height = static_cast<JDIMENSION>(height);
  cinfo->input_components = input_components;
  cinfo->in_color_space = (input_components == 1) ? JCS_GRAYSCALE : JCS_RGB;
  jpegli_set_defaults(cinfo);
  // A JFIF header tells the decoders that the samples are YCbCr, without it
  // old decoders may interpret them as RGB.
  cinfo->write_JFIF_header = TRUE;

  // The chroma subsampling has to be selected before the quantization
  // tables are computed, because jpegli uses different tables for 4:2:0.
  if (input_components == 3 && options.jpeg_subsampling != 0) {
    for (int c = 0; c < cinfo->num_components; ++c) {
      cinfo->comp_info[c].h_samp_factor = 1;
      cinfo->comp_info[c].v_samp_factor = 1;
    }
    if (options.jpeg_subsampling == 2) {  // 4:2:2
      cinfo->comp_info[0].h_samp_factor = 2;
      cinfo->comp_info[0].v_samp_factor = 1;
    } else if (options.jpeg_subsampling == 3) {  // 4:2:0
      cinfo->comp_info[0].h_samp_factor = 2;
      cinfo->comp_info[0].v_samp_factor = 2;
    }
  }

  jpegli_set_distance(cinfo, jpegli_quality_to_distance(quality),
                      /*force_baseline=*/TRUE);
  if (options.jpeg_progressive) {
    jpegli_simple_progression(cinfo);
  }

  jpegli_start_compress(cinfo, TRUE);

  // Feed the image to the encoder row by row.
  const size_t row_size = static_cast<size_t>(width) * input_components;
  auto* rows = reinterpret_cast<JSAMPROW*>(Alloc(sizeof(JSAMPROW) * height));
  if (rows == nullptr) {
    jpegli_destroy_compress(cinfo);
    Free(dest->buffer);
    Free(dest);
    Free(cinfo);
    Free(rgb_pixels);
    SetFailure("out of memory");
    return JXLLIMG_ERR_OUT_OF_MEMORY;
  }
  for (int y = 0; y < height; ++y) {
    rows[y] = const_cast<JSAMPROW>(pixels + static_cast<size_t>(y) * row_size);
  }
  // The encoder may accept fewer lines than requested, so keep writing until
  // the whole image is through.
  JDIMENSION written = 0;
  while (written < static_cast<JDIMENSION>(height)) {
    const JDIMENSION num_written = jpegli_write_scanlines(
        cinfo, rows + written, static_cast<JDIMENSION>(height) - written);
    if (num_written == 0) break;
    written += num_written;
  }
  Free(rows);
  if (written < static_cast<JDIMENSION>(height)) {
    jpegli_destroy_compress(cinfo);
    Free(dest->buffer);
    Free(dest);
    Free(cinfo);
    Free(rgb_pixels);
    SetFailure("could not write the image to the JPEG encoder");
    return JXLLIMG_ERR_ENCODE_FAILED;
  }

  jpegli_finish_compress(cinfo);
  jpegli_destroy_compress(cinfo);

  *out_data = dest->buffer;
  *out_size = dest->written;
  // The destination manager itself is not needed anymore, the buffer is
  // handed over to the caller.
  dest->buffer = nullptr;
  Free(dest);
  Free(cinfo);
  Free(rgb_pixels);
  return JXLLIMG_OK;
}

}  // namespace

jxlimg_status DecodeJpeg(const uint8_t* data, size_t size, bool info_only,
                         Image* out) {
  uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int channels = 0;
  const jxlimg_status status = JpegDecodeImpl(data, size, info_only, &pixels,
                                               &width, &height, &channels);
  if (status != JXLLIMG_OK) return status;
  out->pixels = pixels;
  out->width = width;
  out->height = height;
  out->channels = channels;
  return JXLLIMG_OK;
}

jxlimg_status EncodeJpeg(const uint8_t* pixels, int width, int height,
                         int channels, int quality,
                         const jxlimg_options& options, OutputBuffer* out) {
  uint8_t* data = nullptr;
  size_t size = 0;
  const jxlimg_status status = JpegEncodeImpl(pixels, width, height, channels,
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
