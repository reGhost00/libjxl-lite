// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// Internal declarations of the jxlimg wrapper.  Not part of the public API.

#ifndef JXLLIMG_INTERNAL_H_
#define JXLLIMG_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#include "jxlimg.h"

namespace jxlimg {

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

// Allocates `size` bytes with the currently installed allocator.  Returns
// nullptr on failure.
void* Alloc(size_t size);

// Releases a block obtained from Alloc().  `ptr` may be nullptr.
void Free(void* ptr);

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Stores a human readable description of the current failure.  The format
// string must be a literal, user controlled data has to be passed with "%s".
void SetFailure(const char* format, ...);

// Clears the failure description of the current thread.
void ClearFailure();

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

// A growable byte buffer that uses the installed allocator.
class OutputBuffer {
 public:
  OutputBuffer() = default;
  OutputBuffer(const OutputBuffer&) = delete;
  OutputBuffer& operator=(const OutputBuffer&) = delete;
  ~OutputBuffer() { Free(data_); }

  // Appends `size` bytes, growing the buffer if needed.
  bool Append(const uint8_t* data, size_t size);

  // Makes sure that the buffer can hold at least `capacity` bytes.
  bool Reserve(size_t capacity);

  uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

  // Hands the buffer over to the caller, who becomes responsible for
  // releasing it with Free().
  uint8_t* Release();

 private:
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
};

// ---------------------------------------------------------------------------
// Decoded image
// ---------------------------------------------------------------------------

struct Image {
  // Pixel data with `channels` interleaved samples per pixel, tightly
  // packed, top row first.  Owned by the caller.
  uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  // Number of channels of `pixels`: 1 (gray), 2 (gray+alpha), 3 (RGB) or
  // 4 (RGBA).  This is the "natural" channel count of the source image.
  int channels = 0;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Detects the format of a compressed image without decoding it.
jxlimg_format DetectFormat(const uint8_t* data, size_t size);

// Reads a whole file into `out`.  Returns false on failure.
bool ReadFile(const char* filename, OutputBuffer* out);

// Writes `size` bytes to a file.  Returns false on failure.
bool WriteFile(const char* filename, const uint8_t* data, size_t size);

// Converts interleaved pixel data from `src_channels` to `dst_channels`
// samples per pixel.  Supported combinations are 1, 2, 3 and 4 channels;
// the alpha channel is filled with 0xff when the source has none and dropped
// when the destination does not want it.
void ConvertChannels(const uint8_t* src, int src_channels, uint8_t* dst,
                     int dst_channels, size_t num_pixels);

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

// Decodes a JPEG image.  The result has the natural channel count of the
// source image (1 for grayscale, 3 for color).
//
// When `info_only` is true, only the dimensions are determined and no pixels
// are decoded; `out->pixels` is then NULL.
jxlimg_status DecodeJpeg(const uint8_t* data, size_t size, bool info_only,
                         Image* out);

// Encodes an 8-bit image as JPEG.
jxlimg_status EncodeJpeg(const uint8_t* pixels, int width, int height,
                         int channels, int quality,
                         const jxlimg_options& options, OutputBuffer* out);

// Decodes a JPEG XL image.  The result has the natural channel count of the
// source image (1, 2, 3 or 4).
//
// When `info_only` is true, only the dimensions are determined and no pixels
// are decoded; `out->pixels` is then NULL.
jxlimg_status DecodeJxl(const uint8_t* data, size_t size, bool info_only,
                        Image* out);

// Encodes an 8-bit image as JPEG XL.
jxlimg_status EncodeJxl(const uint8_t* pixels, int width, int height,
                        int channels, int quality,
                        const jxlimg_options& options, OutputBuffer* out);

}  // namespace jxlimg

#endif  // JXLLIMG_INTERNAL_H_
