// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
//
// Self contained test of the jxlimg wrapper.  It does not need gtest, the
// fork does not ship it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "jxlimg.h"
#include "jxlimg.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                  \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(condition)) {                                                   \
      ++g_failures;                                                       \
      fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__,         \
              #condition);                                                \
    }                                                                     \
  } while (0)

#define CHECK_EQ(actual, expected)                                         \
  do {                                                                     \
    ++g_checks;                                                            \
    const auto actual_value = (actual);                                    \
    const auto expected_value = (expected);                                \
    if (!(actual_value == expected_value)) {                               \
      ++g_failures;                                                        \
      fprintf(stderr, "%s:%d: FAILED: %s == %s (%lld != %lld)\n",         \
              __FILE__, __LINE__, #actual, #expected,                      \
              static_cast<long long>(actual_value),                        \
              static_cast<long long>(expected_value));                    \
    }                                                                      \
  } while (0)

// A small test image with a smooth gradient and some structure.
void MakeTestImage(int width, int height, int channels,
                   std::vector<uint8_t>* pixels) {
  pixels->resize(static_cast<size_t>(width) * height * channels);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t* pixel = pixels->data() + (static_cast<size_t>(y) * width + x) *
                                              channels;
      pixel[0] = static_cast<uint8_t>(x * 255 / (width > 1 ? width - 1 : 1));
      if (channels > 1) {
        pixel[1] = static_cast<uint8_t>(y * 255 / (height > 1 ? height - 1 : 1));
      }
      if (channels > 2) {
        pixel[2] = static_cast<uint8_t>((x + y) * 255 / (width + height));
      }
      if (channels > 3) {
        pixel[3] = 0xff;
      }
    }
  }
}

// Returns the maximum absolute difference between two images.
int MaxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                  int channels) {
  if (a.size() != b.size()) return 255;
  int max_diff = 0;
  for (size_t i = 0; i < a.size(); i += channels) {
    for (int c = 0; c < channels; ++c) {
      const int diff = static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]);
      if (diff < 0 ? -diff > max_diff : diff > max_diff) {
        max_diff = diff < 0 ? -diff : diff;
      }
    }
  }
  return max_diff;
}

void TestFormatDetection() {
  const uint8_t jpeg_magic[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10};
  const uint8_t jxl_codestream_magic[] = {0xff, 0x0a, 0x00, 0x00};
  const uint8_t jxl_container_magic[] = {0x00, 0x00, 0x00, 0x0c, 0x4a, 0x58,
                                         0x4c, 0x20, 0x0d, 0x0a, 0x87, 0x0a};
  const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
  CHECK_EQ(jxlimg_format_from_memory(jpeg_magic, sizeof(jpeg_magic)),
           JXLLIMG_FORMAT_JPEG);
  CHECK_EQ(jxlimg_format_from_memory(jxl_codestream_magic,
                                     sizeof(jxl_codestream_magic)),
           JXLLIMG_FORMAT_JXL);
  CHECK_EQ(jxlimg_format_from_memory(jxl_container_magic,
                                     sizeof(jxl_container_magic)),
           JXLLIMG_FORMAT_JXL);
  CHECK_EQ(jxlimg_format_from_memory(garbage, sizeof(garbage)),
           JXLLIMG_FORMAT_UNKNOWN);
  CHECK_EQ(jxlimg_format_from_memory(nullptr, 0), JXLLIMG_FORMAT_UNKNOWN);
  // A JPEG file starts with 0xff 0xd8, a JXL codestream with 0xff 0x0a.
  CHECK_EQ(jxlimg_format_from_memory(jpeg_magic, 1), JXLLIMG_FORMAT_UNKNOWN);
}

void TestRoundTrip(jxlimg_format format, int channels, int tolerance) {
  const int width = 61;
  const int height = 37;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, channels, &source);

  size_t encoded_size = 0;
  uint8_t* encoded = jxlimg_write_to_mem(source.data(), width, height,
                                         channels, format, 90, &encoded_size);
  CHECK(encoded != nullptr);
  if (encoded == nullptr) {
    fprintf(stderr, "  encoding %d channels failed: %s\n", channels,
            jxlimg_failure_reason());
    return;
  }
  CHECK(encoded_size > 0);
  CHECK_EQ(jxlimg_format_from_memory(encoded, encoded_size), format);

  int x = 0;
  int y = 0;
  int decoded_channels = 0;
  uint8_t* decoded = jxlimg_load_from_memory(encoded, encoded_size, &x, &y,
                                             &decoded_channels, 0);
  CHECK(decoded != nullptr);
  if (decoded != nullptr) {
    CHECK_EQ(x, width);
    CHECK_EQ(y, height);
    // JPEG has no alpha channel.
    const int expected_channels = (format == JXLLIMG_FORMAT_JPEG && channels == 4)
                                      ? 3
                                      : channels;
    CHECK_EQ(decoded_channels, expected_channels);
    std::vector<uint8_t> decoded_pixels(decoded,
                                        decoded + static_cast<size_t>(x) * y *
                                                       decoded_channels);
    const int difference =
        MaxDifference(source, decoded_pixels, expected_channels);
    if (difference > tolerance) {
      fprintf(stderr, "  round trip difference %d > %d\n", difference,
              tolerance);
    }
    CHECK(difference <= tolerance);
    jxlimg_image_free(decoded);
  }
  jxlimg_image_free(encoded);
}

void TestDesiredChannels() {
  const int width = 32;
  const int height = 24;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);

  size_t encoded_size = 0;
  uint8_t* encoded = jxlimg_write_to_mem(source.data(), width, height, 3,
                                         JXLLIMG_FORMAT_JPEG, 95, &encoded_size);
  CHECK(encoded != nullptr);
  if (encoded == nullptr) return;

  for (int desired = 1; desired <= 4; ++desired) {
    int x = 0;
    int y = 0;
    int channels = 0;
    uint8_t* decoded = jxlimg_load_from_memory(encoded, encoded_size, &x, &y,
                                               &channels, desired);
    CHECK(decoded != nullptr);
    if (decoded == nullptr) continue;
    CHECK_EQ(x, width);
    CHECK_EQ(y, height);
    CHECK_EQ(channels, desired);
    // The alpha channel of a JPEG is filled with 0xff.
    if (desired == 4) {
      bool all_opaque = true;
      for (size_t i = 3; i < static_cast<size_t>(x) * y * 4; i += 4) {
        if (decoded[i] != 0xff) all_opaque = false;
      }
      CHECK(all_opaque);
    }
    jxlimg_image_free(decoded);
  }
  jxlimg_image_free(encoded);
}

void TestGrayscale() {
  const int width = 20;
  const int height = 10;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 1, &source);
  size_t encoded_size = 0;
  uint8_t* encoded =
      jxlimg_write_to_mem(source.data(), width, height, 1, JXLLIMG_FORMAT_JPEG,
                          95, &encoded_size);
  CHECK(encoded != nullptr);
  if (encoded == nullptr) return;
  int x = 0;
  int y = 0;
  int channels = 0;
  uint8_t* decoded =
      jxlimg_load_from_memory(encoded, encoded_size, &x, &y, &channels, 0);
  CHECK(decoded != nullptr);
  if (decoded != nullptr) {
    CHECK_EQ(channels, 1);
    CHECK_EQ(x, width);
    CHECK_EQ(y, height);
    jxlimg_image_free(decoded);
  }
  jxlimg_image_free(encoded);
}

void TestAlpha() {
  // JPEG XL keeps the alpha channel, JPEG does not.
  const int width = 16;
  const int height = 16;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 4, &source);
  for (size_t i = 3; i < source.size(); i += 4) {
    source[i] = static_cast<uint8_t>((i * 7) & 0xff);
  }
  size_t encoded_size = 0;
  uint8_t* encoded = jxlimg_write_to_mem(source.data(), width, height, 4,
                                         JXLLIMG_FORMAT_JXL, 100, &encoded_size);
  CHECK(encoded != nullptr);
  if (encoded == nullptr) return;
  int x = 0;
  int y = 0;
  int channels = 0;
  uint8_t* decoded = jxlimg_load_from_memory(encoded, encoded_size, &x, &y,
                                             &channels, 4);
  CHECK(decoded != nullptr);
  if (decoded != nullptr) {
    CHECK_EQ(channels, 4);
    bool alpha_ok = true;
    for (size_t i = 0; i < static_cast<size_t>(x) * y; ++i) {
      if (decoded[i * 4 + 3] != source[i * 4 + 3]) alpha_ok = false;
    }
    CHECK(alpha_ok);
    jxlimg_image_free(decoded);
  }
  jxlimg_image_free(encoded);
}

// Broken input must not crash the process, the JPEG backend in particular
// has to report the error instead of calling exit().
void TestBrokenInput() {
  const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                              0x06, 0x07, 0x08, 0x09};
  int x = 0;
  int y = 0;
  int channels = 0;
  CHECK(jxlimg_load_from_memory(garbage, sizeof(garbage), &x, &y, &channels,
                                 0) == nullptr);
  CHECK(jxlimg_failure_reason()[0] != '\0');

  // A JPEG that is cut in the middle.
  const int width = 64;
  const int height = 64;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);
  size_t encoded_size = 0;
  uint8_t* encoded = jxlimg_write_to_mem(source.data(), width, height, 3,
                                         JXLLIMG_FORMAT_JPEG, 90, &encoded_size);
  CHECK(encoded != nullptr);
  if (encoded == nullptr) return;
  for (size_t cut = 1; cut < encoded_size; cut += 37) {
    uint8_t* decoded =
        jxlimg_load_from_memory(encoded, cut, &x, &y, &channels, 0);
    if (decoded != nullptr) {
      jxlimg_image_free(decoded);
    }
  }
  // Corrupt the entropy coded data.
  std::vector<uint8_t> corrupt(encoded, encoded + encoded_size);
  for (size_t i = corrupt.size() / 2; i < corrupt.size(); i += 3) {
    corrupt[i] = static_cast<uint8_t>(0xa5);
  }
  uint8_t* decoded =
      jxlimg_load_from_memory(corrupt.data(), corrupt.size(), &x, &y, &channels,
                              0);
  if (decoded != nullptr) {
    jxlimg_image_free(decoded);
  }
  jxlimg_image_free(encoded);
}

void TestInfo() {
  const int width = 12;
  const int height = 9;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);
  for (int format = 0; format < 2; ++format) {
    const jxlimg_format image_format =
        (format == 0) ? JXLLIMG_FORMAT_JPEG : JXLLIMG_FORMAT_JXL;
    size_t encoded_size = 0;
    uint8_t* encoded = jxlimg_write_to_mem(source.data(), width, height, 3,
                                           image_format, 90, &encoded_size);
    CHECK(encoded != nullptr);
    if (encoded == nullptr) continue;
    int x = 0;
    int y = 0;
    int channels = 0;
    CHECK(jxlimg_info_from_memory(encoded, encoded_size, &x, &y, &channels) ==
          1);
    CHECK_EQ(x, width);
    CHECK_EQ(y, height);
    CHECK_EQ(channels, 3);
    // The info path must not depend on the pixels being decoded.
    CHECK(jxlimg_info_from_memory(encoded, 4, &x, &y, &channels) == 0);
    jxlimg_image_free(encoded);
  }
}

void TestFileIo() {
  const int width = 40;
  const int height = 30;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);
  const char* filename = "jxlimg_test_output.jxl";
  CHECK(jxlimg_write(filename, source.data(), width, height, 3,
                     JXLLIMG_FORMAT_JXL, 90) == 1);
  CHECK_EQ(jxlimg_format_from_file(filename), JXLLIMG_FORMAT_JXL);
  int x = 0;
  int y = 0;
  int channels = 0;
  uint8_t* decoded = jxlimg_load(filename, &x, &y, &channels, 0);
  CHECK(decoded != nullptr);
  CHECK_EQ(x, width);
  CHECK_EQ(y, height);
  jxlimg_image_free(decoded);
  remove(filename);
}

// A custom allocator has to be used for the buffers of the library.
size_t g_allocations = 0;

void* CountingMalloc(void* user_data, size_t size) {
  (void)user_data;
  ++g_allocations;
  return malloc(size);
}

void CountingFree(void* user_data, void* ptr) {
  (void)user_data;
  free(ptr);
}

void TestAllocator() {
  jxlimg_allocator allocator;
  memset(&allocator, 0, sizeof(allocator));
  allocator.malloc_func = CountingMalloc;
  allocator.free_func = CountingFree;
  CHECK_EQ(jxlimg_set_allocator(&allocator), 0);
  // An allocator with only one of the two functions is rejected.
  jxlimg_allocator invalid;
  memset(&invalid, 0, sizeof(invalid));
  invalid.malloc_func = CountingMalloc;
  CHECK_EQ(jxlimg_set_allocator(&invalid), -1);

  g_allocations = 0;
  const int width = 16;
  const int height = 16;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);
  size_t encoded_size = 0;
  uint8_t* encoded = jxlimg_write_to_mem(source.data(), width, height, 3,
                                         JXLLIMG_FORMAT_JPEG, 90, &encoded_size);
  CHECK(encoded != nullptr);
  CHECK(g_allocations > 0);
  if (encoded != nullptr) {
    int x = 0;
    int y = 0;
    int channels = 0;
    uint8_t* decoded = jxlimg_load_from_memory(encoded, encoded_size, &x, &y,
                                               &channels, 0);
    CHECK(decoded != nullptr);
    jxlimg_image_free(decoded);
    jxlimg_image_free(encoded);
  }
  // Restore the default allocator.
  CHECK_EQ(jxlimg_set_allocator(nullptr), 0);
}

// The JPEG encoder options have to work: progressive, different chroma
// subsamplings and a bare JPEG XL codestream.
void TestEncoderOptions() {
  const int width = 33;
  const int height = 21;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);

  for (int subsampling = 0; subsampling <= 3; ++subsampling) {
    for (int progressive = 0; progressive <= 1; ++progressive) {
      jxlimg_options options;
      jxlimg_options_init(&options);
      options.quality = 85;
      options.jpeg_subsampling = subsampling;
      options.jpeg_progressive = progressive;
      size_t encoded_size = 0;
      uint8_t* encoded = jxlimg_write_to_mem_ex(
          source.data(), width, height, 3, JXLLIMG_FORMAT_JPEG, &options,
          &encoded_size);
      CHECK(encoded != nullptr);
      if (encoded == nullptr) {
        fprintf(stderr, "  subsampling %d progressive %d failed: %s\n",
                subsampling, progressive, jxlimg_failure_reason());
        continue;
      }
      int x = 0;
      int y = 0;
      int channels = 0;
      uint8_t* decoded = jxlimg_load_from_memory(encoded, encoded_size, &x, &y,
                                                 &channels, 0);
      CHECK(decoded != nullptr);
      if (decoded != nullptr) {
        CHECK_EQ(x, width);
        CHECK_EQ(y, height);
        CHECK_EQ(channels, 3);
        std::vector<uint8_t> decoded_pixels(
            decoded, decoded + static_cast<size_t>(x) * y * 3);
        CHECK(MaxDifference(source, decoded_pixels, 3) <= 60);
        jxlimg_image_free(decoded);
      }
      jxlimg_image_free(encoded);
    }
  }

  // A bare JPEG XL codestream instead of a container.
  jxlimg_options options;
  jxlimg_options_init(&options);
  options.jxl_container = 0;
  size_t encoded_size = 0;
  uint8_t* encoded = jxlimg_write_to_mem_ex(
      source.data(), width, height, 3, JXLLIMG_FORMAT_JXL, &options,
      &encoded_size);
  CHECK(encoded != nullptr);
  if (encoded != nullptr) {
    CHECK_EQ(jxlimg_format_from_memory(encoded, encoded_size),
             JXLLIMG_FORMAT_JXL);
    int x = 0;
    int y = 0;
    int channels = 0;
    uint8_t* decoded = jxlimg_load_from_memory(encoded, encoded_size, &x, &y,
                                               &channels, 0);
    CHECK(decoded != nullptr);
    if (decoded != nullptr) {
      CHECK_EQ(x, width);
      CHECK_EQ(y, height);
      jxlimg_image_free(decoded);
    }
    jxlimg_image_free(encoded);
  }
}

void TestStatusStrings() {
  CHECK(jxlimg_status_string(JXLLIMG_OK) != nullptr);
  CHECK(jxlimg_status_string(JXLLIMG_ERR_DECODE_FAILED) != nullptr);
  CHECK(strlen(jxlimg_status_string(JXLLIMG_ERR_OUT_OF_MEMORY)) > 0);
}

// The optional C++ header has to work as well.
void TestCppHeader() {
  const int width = 24;
  const int height = 16;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);

  jxlimg::Image image;
  image.width = width;
  image.height = height;
  image.channels = 3;
  image.pixels = source;

  std::vector<uint8_t> encoded;
  CHECK(jxlimg::Encode(image, JXLLIMG_FORMAT_JXL, &encoded, 90));
  CHECK(!encoded.empty());

  jxlimg::Image decoded;
  CHECK(jxlimg::LoadFromMemory(encoded.data(), encoded.size(), &decoded, 0));
  CHECK_EQ(decoded.width, width);
  CHECK_EQ(decoded.height, height);
  CHECK_EQ(decoded.channels, 3);
  CHECK_EQ(decoded.pixels.size(), source.size());
}

void RoundTripJpeg3() { TestRoundTrip(JXLLIMG_FORMAT_JPEG, 3, 40); }
// JPEG XL uses the perceptual XYB transform by default, so the round trip
// error is larger than for JPEG even though the images look the same.
void RoundTripJxl3() { TestRoundTrip(JXLLIMG_FORMAT_JXL, 3, 30); }
void RoundTripJpeg1() { TestRoundTrip(JXLLIMG_FORMAT_JPEG, 1, 40); }
void RoundTripJxl1() { TestRoundTrip(JXLLIMG_FORMAT_JXL, 1, 30); }

// A lossless JPEG XL has to reproduce the pixels exactly.
void TestLossless() {
  const int width = 37;
  const int height = 23;
  std::vector<uint8_t> source;
  MakeTestImage(width, height, 3, &source);
  jxlimg_options options;
  jxlimg_options_init(&options);
  options.quality = 100;
  options.jxl_lossless = 1;
  size_t encoded_size = 0;
  uint8_t* encoded = jxlimg_write_to_mem_ex(source.data(), width, height, 3,
                                             JXLLIMG_FORMAT_JXL, &options,
                                             &encoded_size);
  CHECK(encoded != nullptr);
  if (encoded == nullptr) {
    fprintf(stderr, "  lossless encoding failed: %s\n",
            jxlimg_failure_reason());
    return;
  }
  int x = 0;
  int y = 0;
  int channels = 0;
  uint8_t* decoded = jxlimg_load_from_memory(encoded, encoded_size, &x, &y,
                                             &channels, 0);
  CHECK(decoded != nullptr);
  if (decoded != nullptr) {
    CHECK_EQ(x, width);
    CHECK_EQ(y, height);
    CHECK_EQ(channels, 3);
    std::vector<uint8_t> decoded_pixels(decoded,
                                        decoded + static_cast<size_t>(x) * y * 3);
    const int difference = MaxDifference(source, decoded_pixels, 3);
    if (difference != 0) {
      fprintf(stderr, "  lossless round trip difference %d\n", difference);
    }
    CHECK_EQ(difference, 0);
    jxlimg_image_free(decoded);
  }
  jxlimg_image_free(encoded);
}

struct TestCase {
  const char* name;
  void (*run)();
};

const TestCase kTests[] = {
    {"format", TestFormatDetection},
    {"roundtrip-jpeg3", RoundTripJpeg3},
    {"roundtrip-jxl3", RoundTripJxl3},
    {"roundtrip-jpeg1", RoundTripJpeg1},
    {"roundtrip-jxl1", RoundTripJxl1},
    {"lossless", TestLossless},
    {"channels", TestDesiredChannels},
    {"grayscale", TestGrayscale},
    {"alpha", TestAlpha},
    {"broken", TestBrokenInput},
    {"info", TestInfo},
    {"file", TestFileIo},
    {"options", TestEncoderOptions},
    {"status", TestStatusStrings},
    {"cpp", TestCppHeader},
    {"allocator", TestAllocator},
};

}  // namespace

int main(int argc, char** argv) {
  // The optional argument selects a single test, which is handy when
  // debugging.
  const char* only = (argc > 1) ? argv[1] : nullptr;
  bool ran_something = false;
  for (const TestCase& test : kTests) {
    if (only != nullptr && strcmp(only, test.name) != 0) continue;
    ran_something = true;
    test.run();
  }
  if (!ran_something) {
    fprintf(stderr, "no such test: %s\n", only);
    return 2;
  }
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
