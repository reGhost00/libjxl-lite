// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// Format detection, memory management and the public entry points of jxlimg.

#include "jxlimg_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "jxl/decode.h"

namespace jxlimg {
namespace {

void* DefaultMalloc(void* user_data, size_t size) {
  (void)user_data;
  return malloc(size);
}

void* DefaultRealloc(void* user_data, void* ptr, size_t size) {
  (void)user_data;
  return realloc(ptr, size);
}

void DefaultFree(void* user_data, void* ptr) {
  (void)user_data;
  free(ptr);
}

struct AllocatorState {
  jxlimg_malloc_func malloc_func = DefaultMalloc;
  // nullptr means "emulate realloc with malloc + memcpy + free", which is
  // what happens when the user provides a custom allocator without a
  // realloc function.
  jxlimg_realloc_func realloc_func = DefaultRealloc;
  jxlimg_free_func free_func = DefaultFree;
  void* user_data = nullptr;
};

// The allocator is global, like the one of stb_image.  It has to be
// installed before the first call into the library.
AllocatorState g_allocator;

thread_local char g_failure_reason[256] = {};

}  // namespace

void* Alloc(size_t size) {
  return g_allocator.malloc_func(g_allocator.user_data, size);
}

void Free(void* ptr) {
  if (ptr != nullptr) {
    g_allocator.free_func(g_allocator.user_data, ptr);
  }
}

void SetFailure(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(g_failure_reason, sizeof(g_failure_reason), format, args);
  va_end(args);
}

void ClearFailure() { g_failure_reason[0] = '\0'; }

bool OutputBuffer::Reserve(size_t capacity) {
  if (capacity <= capacity_) return true;
  size_t new_capacity = capacity_ == 0 ? 4096 : capacity_;
  while (new_capacity < capacity) {
    if (new_capacity > (size_t)-1 / 2) {
      new_capacity = capacity;
      break;
    }
    new_capacity *= 2;
  }
  uint8_t* new_data = nullptr;
  if (g_allocator.realloc_func != nullptr) {
    new_data = reinterpret_cast<uint8_t*>(
        g_allocator.realloc_func(g_allocator.user_data, data_, new_capacity));
    if (new_data == nullptr) return false;
  } else {
    new_data = reinterpret_cast<uint8_t*>(Alloc(new_capacity));
    if (new_data == nullptr) return false;
    if (data_ != nullptr) {
      memcpy(new_data, data_, size_);
      Free(data_);
    }
  }
  data_ = new_data;
  capacity_ = new_capacity;
  return true;
}

bool OutputBuffer::Append(const uint8_t* data, size_t size) {
  if (size == 0) return true;
  if (!Reserve(size_ + size)) return false;
  memcpy(data_ + size_, data, size);
  size_ += size;
  return true;
}

uint8_t* OutputBuffer::Release() {
  uint8_t* result = data_;
  data_ = nullptr;
  size_ = 0;
  capacity_ = 0;
  return result;
}

jxlimg_format DetectFormat(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 2) return JXLLIMG_FORMAT_UNKNOWN;
  // JPEG starts with SOI (0xff 0xd8).
  if (data[0] == 0xff && data[1] == 0xd8) return JXLLIMG_FORMAT_JPEG;
  JxlSignature signature = JxlSignatureCheck(data, size);
  if (signature == JXL_SIG_CODESTREAM || signature == JXL_SIG_CONTAINER) {
    return JXLLIMG_FORMAT_JXL;
  }
  return JXLLIMG_FORMAT_UNKNOWN;
}

bool ReadFile(const char* filename, OutputBuffer* out) {
  FILE* file = fopen(filename, "rb");
  if (file == nullptr) {
    SetFailure("could not open '%s' for reading", filename);
    return false;
  }
  uint8_t chunk[64 * 1024];
  bool ok = true;
  for (;;) {
    size_t read = fread(chunk, 1, sizeof(chunk), file);
    if (read > 0 && !out->Append(chunk, read)) {
      SetFailure("out of memory while reading '%s'", filename);
      ok = false;
      break;
    }
    if (read < sizeof(chunk)) {
      if (ferror(file)) {
        SetFailure("error while reading '%s'", filename);
        ok = false;
      }
      break;
    }
  }
  fclose(file);
  return ok;
}

bool WriteFile(const char* filename, const uint8_t* data, size_t size) {
  FILE* file = fopen(filename, "wb");
  if (file == nullptr) {
    SetFailure("could not open '%s' for writing", filename);
    return false;
  }
  bool ok = true;
  if (size > 0 && fwrite(data, 1, size, file) != size) {
    SetFailure("error while writing '%s'", filename);
    ok = false;
  }
  if (fclose(file) != 0) {
    SetFailure("error while closing '%s'", filename);
    ok = false;
  }
  return ok;
}

void ConvertChannels(const uint8_t* src, int src_channels, uint8_t* dst,
                     int dst_channels, size_t num_pixels) {  if (src_channels == dst_channels) {
    if (num_pixels > 0) {
      memcpy(dst, src, num_pixels * src_channels);
    }
    return;
  }
  for (size_t i = 0; i < num_pixels; ++i) {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a = 0xff;
    switch (src_channels) {
      case 1:
        r = g = b = src[0];
        break;
      case 2:
        r = g = b = src[0];
        a = src[1];
        break;
      case 3:
        r = src[0];
        g = src[1];
        b = src[2];
        break;
      default:
        r = src[0];
        g = src[1];
        b = src[2];
        a = src[3];
        break;
    }
    // ITU-R BT.601 luma, the same definition that libjpeg uses.
    const uint8_t gray =
        static_cast<uint8_t>((77 * r + 150 * g + 29 * b + 128) >> 8);
    switch (dst_channels) {
      case 1:
        dst[0] = gray;
        break;
      case 2:
        dst[0] = gray;
        dst[1] = a;
        break;
      case 3:
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        break;
      default:
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = a;
        break;
    }
    src += src_channels;
    dst += dst_channels;
  }
}

// Decodes an image with the backend that matches its format.  When
// `info_only` is true, only the dimensions are determined.
jxlimg_status DecodeImage(const uint8_t* data, size_t size, bool info_only,
                           Image* image) {
  switch (DetectFormat(data, size)) {
    case JXLLIMG_FORMAT_JPEG:
      return DecodeJpeg(data, size, info_only, image);
    case JXLLIMG_FORMAT_JXL:
      return DecodeJxl(data, size, info_only, image);
    default:
      SetFailure("unsupported image format");
      return JXLLIMG_ERR_UNSUPPORTED_FORMAT;
  }
}

}  // namespace jxlimg

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace jxlimg {
extern "C" {

const char* jxlimg_status_string(jxlimg_status status) {
  switch (status) {
    case JXLLIMG_OK:
      return "success";
    case JXLLIMG_ERR_UNSUPPORTED_FORMAT:
      return "unsupported image format";
    case JXLLIMG_ERR_BAD_ARGUMENT:
      return "invalid argument";
    case JXLLIMG_ERR_DECODE_FAILED:
      return "decoding failed";
    case JXLLIMG_ERR_ENCODE_FAILED:
      return "encoding failed";
    case JXLLIMG_ERR_OUT_OF_MEMORY:
      return "out of memory";
    case JXLLIMG_ERR_INTERNAL:
      return "internal error";
  }
  return "unknown error";
}

int jxlimg_set_allocator(const jxlimg_allocator* allocator) {
  if (allocator == nullptr) {
    g_allocator.malloc_func = DefaultMalloc;
    g_allocator.realloc_func = DefaultRealloc;
    g_allocator.free_func = DefaultFree;
    g_allocator.user_data = nullptr;
    return 0;
  }
  // A custom allocator needs both an allocation and a matching deallocation
  // function, otherwise memory would be released with the wrong allocator.
  if ((allocator->malloc_func == nullptr) != (allocator->free_func == nullptr)) {
    return -1;
  }
  if (allocator->malloc_func == nullptr) {
    g_allocator.malloc_func = DefaultMalloc;
    g_allocator.realloc_func = DefaultRealloc;
    g_allocator.free_func = DefaultFree;
    g_allocator.user_data = nullptr;
    return 0;
  }
  g_allocator.malloc_func = allocator->malloc_func;
  g_allocator.realloc_func = allocator->realloc_func;
  g_allocator.free_func = allocator->free_func;
  g_allocator.user_data = allocator->user_data;
  return 0;
}

const char* jxlimg_failure_reason(void) { return g_failure_reason; }

jxlimg_format jxlimg_format_from_memory(const void* data, size_t size) {
  return jxlimg::DetectFormat(static_cast<const uint8_t*>(data), size);
}

jxlimg_format jxlimg_format_from_file(const char* filename) {
  if (filename == nullptr) {
    jxlimg::SetFailure("invalid argument");
    return JXLLIMG_FORMAT_UNKNOWN;
  }
  jxlimg::ClearFailure();
  jxlimg::OutputBuffer buffer;
  if (!jxlimg::ReadFile(filename, &buffer)) return JXLLIMG_FORMAT_UNKNOWN;
  return jxlimg::DetectFormat(buffer.data(), buffer.size());
}

void jxlimg_options_init(jxlimg_options* options) {
  if (options == nullptr) return;
  options->desired_channels = 0;
  options->quality = 90;
  options->jpeg_progressive = 0;
  options->jpeg_subsampling = 0;
  options->jxl_effort = 7;
  options->jxl_lossless = 0;
  options->jxl_container = 1;
}

void jxlimg_image_free(void* ptr) { jxlimg::Free(ptr); }

unsigned char* jxlimg_load_from_memory_ex(const void* data, size_t size,
                                          const jxlimg_options* options, int* x,
                                          int* y, int* channels_in_file) {
  jxlimg::ClearFailure();
  if (data == nullptr || size == 0) {
    jxlimg::SetFailure("invalid argument");
    return nullptr;
  }
  jxlimg_options opts;
  jxlimg_options_init(&opts);
  if (options != nullptr) opts = *options;
  if (opts.desired_channels < 0 || opts.desired_channels > 4) {
    jxlimg::SetFailure("invalid desired_channels %d", opts.desired_channels);
    return nullptr;
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  jxlimg::Image image;
  const jxlimg_status status = jxlimg::DecodeImage(bytes, size, false, &image);
  if (status != JXLLIMG_OK) {
    jxlimg::Free(image.pixels);
    return nullptr;
  }
  const int source_channels = image.channels;
  const int target_channels =
      opts.desired_channels == 0 ? source_channels : opts.desired_channels;
  if (target_channels != source_channels) {
    const size_t num_pixels =
        static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    uint8_t* converted =
        reinterpret_cast<uint8_t*>(jxlimg::Alloc(num_pixels * target_channels));
    if (converted == nullptr) {
      jxlimg::Free(image.pixels);
      jxlimg::SetFailure("out of memory");
      return nullptr;
    }
    jxlimg::ConvertChannels(image.pixels, source_channels, converted,
                            target_channels, num_pixels);
    jxlimg::Free(image.pixels);
    image.pixels = converted;
    image.channels = target_channels;
  }
  if (x != nullptr) *x = image.width;
  if (y != nullptr) *y = image.height;
  if (channels_in_file != nullptr) *channels_in_file = image.channels;
  return image.pixels;
}

unsigned char* jxlimg_load_from_memory(const void* data, size_t size, int* x,
                                       int* y, int* channels_in_file,
                                       int desired_channels) {
  jxlimg_options options;
  jxlimg_options_init(&options);
  options.desired_channels = desired_channels;
  return jxlimg_load_from_memory_ex(data, size, &options, x, y,
                                    channels_in_file);
}

unsigned char* jxlimg_load(const char* filename, int* x, int* y,
                            int* channels_in_file, int desired_channels) {
  jxlimg::ClearFailure();
  if (filename == nullptr) {
    jxlimg::SetFailure("invalid argument");
    return nullptr;
  }
  jxlimg::OutputBuffer buffer;
  if (!jxlimg::ReadFile(filename, &buffer)) return nullptr;
  return jxlimg_load_from_memory(buffer.data(), buffer.size(), x, y,
                                 channels_in_file, desired_channels);
}

int jxlimg_info_from_memory(const void* data, size_t size, int* x, int* y,
                             int* channels_in_file) {
  if (data == nullptr || size == 0) {
    jxlimg::ClearFailure();
    jxlimg::SetFailure("invalid argument");
    return 0;
  }
  jxlimg::ClearFailure();
  jxlimg::Image image;
  const jxlimg_status status = jxlimg::DecodeImage(
      static_cast<const uint8_t*>(data), size, true, &image);
  if (status != JXLLIMG_OK) return 0;
  if (x != nullptr) *x = image.width;
  if (y != nullptr) *y = image.height;
  if (channels_in_file != nullptr) *channels_in_file = image.channels;
  return 1;
}

int jxlimg_info(const char* filename, int* x, int* y, int* channels_in_file) {
  jxlimg::ClearFailure();
  if (filename == nullptr) {
    jxlimg::SetFailure("invalid argument");
    return 0;
  }
  jxlimg::OutputBuffer buffer;
  if (!jxlimg::ReadFile(filename, &buffer)) return 0;
  return jxlimg_info_from_memory(buffer.data(), buffer.size(), x, y,
                                 channels_in_file);
}

unsigned char* jxlimg_write_to_mem_ex(const void* pixels, int width, int height,
                                      int channels, jxlimg_format format,
                                      const jxlimg_options* options,
                                      size_t* out_size) {
  jxlimg::ClearFailure();
  if (pixels == nullptr || width <= 0 || height <= 0) {
    jxlimg::SetFailure("invalid argument");
    return nullptr;
  }
  if (channels != 1 && channels != 3 && channels != 4) {
    jxlimg::SetFailure("invalid number of channels %d", channels);
    return nullptr;
  }
  jxlimg_options opts;
  jxlimg_options_init(&opts);
  if (options != nullptr) opts = *options;
  if (opts.jpeg_subsampling < 0 || opts.jpeg_subsampling > 3) {
    jxlimg::SetFailure("invalid jpeg_subsampling %d", opts.jpeg_subsampling);
    return nullptr;
  }
  if (opts.jxl_effort < 1 || opts.jxl_effort > 9) {
    jxlimg::SetFailure("invalid jxl_effort %d", opts.jxl_effort);
    return nullptr;
  }
  if (opts.quality < 0 || opts.quality > 100) {
    jxlimg::SetFailure("invalid quality %d", opts.quality);
    return nullptr;
  }
  if (format != JXLLIMG_FORMAT_JPEG && format != JXLLIMG_FORMAT_JXL) {
    jxlimg::SetFailure("unsupported output format");
    return nullptr;
  }
  const int quality = opts.quality == 0 ? 90 : opts.quality;
  jxlimg::OutputBuffer buffer;
  jxlimg_status status;
  const uint8_t* source = static_cast<const uint8_t*>(pixels);
  if (format == JXLLIMG_FORMAT_JPEG) {
    status = jxlimg::EncodeJpeg(source, width, height, channels, quality, opts,
                                &buffer);
  } else {
    status = jxlimg::EncodeJxl(source, width, height, channels, quality, opts,
                               &buffer);
  }
  if (status != JXLLIMG_OK) return nullptr;
  if (out_size != nullptr) *out_size = buffer.size();
  return buffer.Release();
}

unsigned char* jxlimg_write_to_mem(const void* pixels, int width, int height,
                                   int channels, jxlimg_format format,
                                   int quality, size_t* out_size) {
  jxlimg_options options;
  jxlimg_options_init(&options);
  options.quality = quality;
  return jxlimg_write_to_mem_ex(pixels, width, height, channels, format,
                                &options, out_size);
}

int jxlimg_write(const char* filename, const void* pixels, int width,
                 int height, int channels, jxlimg_format format, int quality) {
  jxlimg::ClearFailure();
  if (filename == nullptr) {
    jxlimg::SetFailure("invalid argument");
    return 0;
  }
  size_t size = 0;
  unsigned char* encoded =
      jxlimg_write_to_mem(pixels, width, height, channels, format, quality,
                          &size);
  if (encoded == nullptr) return 0;
  bool ok = jxlimg::WriteFile(filename, encoded, size);
  jxlimg::Free(encoded);
  return ok ? 1 : 0;
}

}  // extern "C"
}  // namespace jxlimg
