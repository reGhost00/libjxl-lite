// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// Public C API of jxlimg, a small unified image codec that reads and writes
// both JPEG (via the in-tree lib/jpegli/) and JPEG XL (via lib/jxl/).
//
// The API follows the conventions of stb_image / stb_image_write:
//   * one-shot entry points, no decoder object to manage,
//   * a `NULL` return value means failure, the reason is available through
//     jxlimg_failure_reason(),
//   * buffers returned by the library are released with jxlimg_image_free().
//
// All functions are thread-safe, except for jxlimg_set_allocator(), which
// must be called before any other function of this library and must not be
// called concurrently with other functions.

#ifndef JXLLIMG_H_
#define JXLLIMG_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Image container formats understood by this library. */
typedef enum {
  JXLLIMG_FORMAT_UNKNOWN = 0,
  /** JPEG, as produced by lib/jpegli/ (libjpeg62 compatible). */
  JXLLIMG_FORMAT_JPEG = 1,
  /** JPEG XL, either a container ("\x00\x00\x00\x0CJXL ...") or a bare
   *  codestream ("\xFF\x0A"). */
  JXLLIMG_FORMAT_JXL = 2,
} jxlimg_format;

/** Error codes. jxlimg_status_string() maps them to a human readable
 *  description. */
typedef enum {
  JXLLIMG_OK = 0,
  /** The data is neither JPEG nor JPEG XL. */
  JXLLIMG_ERR_UNSUPPORTED_FORMAT = 1,
  /** A parameter was out of range or inconsistent. */
  JXLLIMG_ERR_BAD_ARGUMENT = 2,
  /** The compressed data could not be decoded. */
  JXLLIMG_ERR_DECODE_FAILED = 3,
  /** The image could not be encoded. */
  JXLLIMG_ERR_ENCODE_FAILED = 4,
  /** Out of memory. */
  JXLLIMG_ERR_OUT_OF_MEMORY = 5,
  /** Unexpected internal failure. */
  JXLLIMG_ERR_INTERNAL = 6,
} jxlimg_status;

/** Human readable description of a status code. Never returns NULL. */
const char* jxlimg_status_string(jxlimg_status status);

/* -------------------------------------------------------------------------
 * Custom memory allocator
 * ------------------------------------------------------------------------- */

/** Allocates `size` bytes. Returns NULL on failure. */
typedef void* (*jxlimg_malloc_func)(void* user_data, size_t size);
/** Resizes a block previously returned by jxlimg_malloc_func. `ptr` may be
 *  NULL, in which case this behaves like jxlimg_malloc_func. */
typedef void* (*jxlimg_realloc_func)(void* user_data, void* ptr, size_t size);
/** Releases a block previously returned by jxlimg_malloc_func or
 *  jxlimg_realloc_func. `ptr` may be NULL, in which case this must do
 *  nothing. */
typedef void (*jxlimg_free_func)(void* user_data, void* ptr);

/** Set of functions used by jxlimg to allocate memory.
 *
 *  `malloc_func` and `free_func` must either both be set or both be NULL.
 *  `realloc_func` may be NULL, in which case jxlimg emulates it with
 *  malloc + memcpy + free.
 *
 *  The allocator is used for the pixel buffers and the compressed buffers
 *  returned by this library, and is forwarded to the JPEG XL decoder and
 *  encoder. */
typedef struct {
  jxlimg_malloc_func malloc_func;
  jxlimg_realloc_func realloc_func;
  jxlimg_free_func free_func;
  /** Opaque pointer passed to all of the functions above. */
  void* user_data;
} jxlimg_allocator;

/** Installs a custom memory allocator.
 *
 *  Passing NULL restores the default (malloc/realloc/free).  Returns 0 on
 *  success and -1 if the allocator is invalid (e.g. only one of
 *  `malloc_func` / `free_func` was provided).
 *
 *  This must be called before any other function of this library, and must
 *  not be called concurrently with other functions. */
int jxlimg_set_allocator(const jxlimg_allocator* allocator);

/* -------------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------------- */

/** Returns a human readable description of the most recent failure in the
 *  current thread, or an empty string if there was none.  Never returns
 *  NULL. */
const char* jxlimg_failure_reason(void);

/* -------------------------------------------------------------------------
 * Format detection
 * ------------------------------------------------------------------------- */

/** Detects the format of a compressed image without decoding it.  Returns
 *  JXLLIMG_FORMAT_UNKNOWN if the data is not a supported image. */
jxlimg_format jxlimg_format_from_memory(const void* data, size_t size);

/** Same as jxlimg_format_from_memory(), but reads the file first. */
jxlimg_format jxlimg_format_from_file(const char* filename);

/* -------------------------------------------------------------------------
 * Decoding
 * ------------------------------------------------------------------------- */

/** Decodes a compressed image that is already in memory.
 *
 *  @param data      compressed image data.
 *  @param size      size of `data` in bytes.
 *  @param x         receives the image width, may be NULL.
 *  @param y         receives the image height, may be NULL.
 *  @param channels_in_file
 *                   receives the number of channels of the decoded image
 *                   (1 for grayscale, 3 for RGB, 4 for RGBA), may be NULL.
 *  @param desired_channels
 *                   0 keeps the channels of the source image, 1 requests
 *                   grayscale, 3 requests RGB, 4 requests RGBA.  Grayscale
 *                   and RGBA output are not supported for every source
 *                   format; JXLLIMG_ERR_BAD_ARGUMENT is returned then.
 *  @return a newly allocated buffer with `x * y * channels_in_file` bytes,
 *          or NULL on failure.  Release it with jxlimg_image_free().
 */
unsigned char* jxlimg_load_from_memory(const void* data, size_t size,
                                       int* x, int* y, int* channels_in_file,
                                       int desired_channels);

/** Decodes a compressed image from a file.  See
 *  jxlimg_load_from_memory() for the parameters. */
unsigned char* jxlimg_load(const char* filename, int* x, int* y,
                            int* channels_in_file, int desired_channels);

/** Reads the dimensions of a compressed image without decoding the pixels.
 *  Returns 1 on success and 0 on failure. */
int jxlimg_info_from_memory(const void* data, size_t size, int* x, int* y,
                             int* channels_in_file);

/** Reads the dimensions of a compressed image file without decoding the
 *  pixels.  Returns 1 on success and 0 on failure. */
int jxlimg_info(const char* filename, int* x, int* y, int* channels_in_file);

/** Releases a buffer returned by this library.  `ptr` may be NULL. */
void jxlimg_image_free(void* ptr);

/* -------------------------------------------------------------------------
 * Encoding
 * ------------------------------------------------------------------------- */

/** Encodes an 8-bit image into a compressed image in memory.
 *
 *  @param pixels    image data, `width * height * channels` bytes, tightly
 *                   packed, top row first.
 *  @param width     image width in pixels, must be > 0.
 *  @param height    image height in pixels, must be > 0.
 *  @param channels  number of channels of `pixels`: 1 (grayscale), 3 (RGB) or
 *                   4 (RGBA).  JPEG has no alpha channel, the alpha channel
 *                   of an RGBA image is dropped when encoding to JPEG.
 *  @param format    JXLLIMG_FORMAT_JPEG or JXLLIMG_FORMAT_JXL.
 *  @param quality   quality between 1 and 100, 0 selects the default (90).
 *  @param out_size  receives the size of the returned buffer, may be NULL.
 *  @return a newly allocated buffer with the compressed image, or NULL on
 *          failure.  Release it with jxlimg_image_free().
 */
unsigned char* jxlimg_write_to_mem(const void* pixels, int width, int height,
                                   int channels, jxlimg_format format,
                                   int quality, size_t* out_size);

/** Encodes an 8-bit image into a compressed image file.  See
 *  jxlimg_write_to_mem() for the parameters.  Returns 1 on success and 0 on
 *  failure. */
int jxlimg_write(const char* filename, const void* pixels, int width,
                 int height, int channels, jxlimg_format format, int quality);

/* -------------------------------------------------------------------------
 * Advanced options
 * ------------------------------------------------------------------------- */

/** Options for the "_ex" variants of the entry points above. */
typedef struct {
  /** 0 keeps the channels of the source image, 1 grayscale, 3 RGB, 4 RGBA. */
  int desired_channels;
  /** Encoding quality between 1 and 100, 0 selects the default (90). */
  int quality;
  /** Encode as a progressive JPEG.  Ignored for JPEG XL. */
  int jpeg_progressive;
  /** Chroma subsampling of the JPEG encoder:
   *  0 = automatic (4:2:0), 1 = 4:4:4, 2 = 4:2:2, 3 = 4:2:0. */
  int jpeg_subsampling;
  /** JPEG XL encoding effort, 1 (fastest) to 9 (slowest).  Default 7. */
  int jxl_effort;
  /** Encode a mathematically lossless JPEG XL.  Ignored for JPEG. */
  int jxl_lossless;
  /** Write a JPEG XL container instead of a bare codestream.  Default 1. */
  int jxl_container;
} jxlimg_options;

/** Fills `options` with the default values. */
void jxlimg_options_init(jxlimg_options* options);

/** Same as jxlimg_load_from_memory(), but with explicit options.  `options`
 *  may be NULL, in which case the defaults are used. */
unsigned char* jxlimg_load_from_memory_ex(const void* data, size_t size,
                                          const jxlimg_options* options, int* x,
                                          int* y, int* channels_in_file);

/** Same as jxlimg_write_to_mem(), but with explicit options.  `options` may
 *  be NULL, in which case the defaults are used. */
unsigned char* jxlimg_write_to_mem_ex(const void* pixels, int width,
                                      int height, int channels,
                                      jxlimg_format format,
                                      const jxlimg_options* options,
                                      size_t* out_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // JXLLIMG_H_
