// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// Optional C++ convenience header for jxlimg.  It is header only and does
// not need any additional library.

#ifndef JXLLIMG_HPP_
#define JXLLIMG_HPP_

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "jxlimg.h"

namespace jxlimg {

/** An uncompressed 8-bit image. */
struct Image {
  int width = 0;
  int height = 0;
  /** Number of interleaved channels: 1, 2, 3 or 4. */
  int channels = 0;
  std::vector<uint8_t> pixels;

  size_t pixel_count() const {
    return static_cast<size_t>(width) * static_cast<size_t>(height);
  }
};

/** Decodes an image that is already in memory.
 *
 *  @param desired_channels 0 keeps the channels of the source image, 1
 *         grayscale, 3 RGB, 4 RGBA.
 *  @return true on success. */
inline bool LoadFromMemory(const void* data, size_t size, Image* image,
                           int desired_channels = 0) {
  if (image == nullptr) return false;
  int x = 0;
  int y = 0;
  int channels = 0;
  uint8_t* pixels =
      jxlimg_load_from_memory(data, size, &x, &y, &channels, desired_channels);
  if (pixels == nullptr) return false;
  image->width = x;
  image->height = y;
  image->channels = channels;
  image->pixels.assign(pixels, pixels + image->pixel_count() * channels);
  jxlimg_image_free(pixels);
  return true;
}

/** Decodes an image file.  @see LoadFromMemory */
inline bool LoadFile(const char* filename, Image* image,
                     int desired_channels = 0) {
  if (image == nullptr || filename == nullptr) return false;
  int x = 0;
  int y = 0;
  int channels = 0;
  uint8_t* pixels =
      jxlimg_load(filename, &x, &y, &channels, desired_channels);
  if (pixels == nullptr) return false;
  image->width = x;
  image->height = y;
  image->channels = channels;
  image->pixels.assign(pixels, pixels + image->pixel_count() * channels);
  jxlimg_image_free(pixels);
  return true;
}

/** Encodes an image into a compressed image in memory.
 *
 *  @param quality between 1 and 100, 0 selects the default (90).
 *  @return true on success. */
inline bool Encode(const Image& image, jxlimg_format format,
                   std::vector<uint8_t>* out, int quality = 90) {
  if (out == nullptr || image.width <= 0 || image.height <= 0) return false;
  size_t size = 0;
  uint8_t* encoded = jxlimg_write_to_mem(image.pixels.data(), image.width,
                                          image.height, image.channels, format,
                                          quality, &size);
  if (encoded == nullptr) return false;
  out->assign(encoded, encoded + size);
  jxlimg_image_free(encoded);
  return true;
}

/** Encodes an image into a compressed image file.  @see Encode */
inline bool EncodeFile(const char* filename, const Image& image,
                       jxlimg_format format, int quality = 90) {
  if (filename == nullptr || image.width <= 0 || image.height <= 0) {
    return false;
  }
  return jxlimg_write(filename, image.pixels.data(), image.width, image.height,
                      image.channels, format, quality) == 1;
}

}  // namespace jxlimg

#endif  // JXLLIMG_HPP_
