// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// JPEG XL CLI Tool with ImageMagick support for image format conversion
// Supports progressive encoding/decoding and animation encoding

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstdio>

#ifdef JXLL_BUILD_WITH_IMAGEMAGICK
#include <Magick++.h>
#elif defined(JXLL_BUILD_WITH_WIC)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <shcore.h>
#include <wincodec.h>
#include <windows.h>
#else
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#endif

// libjxl headers
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>
#include <jxl/types.h>

#include "tinyfiledialogs.h"

// File operations
static bool ReadFile(const char* filename, uint8_t** data, size_t* size) {
  FILE* file = fopen(filename, "rb");
  if (!file) {
    fprintf(stderr, "Could not open %s for reading\n", filename);
    return false;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }

  long fsize = ftell(file);
  if (fsize <= 0) {
    fclose(file);
    return false;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }

  *data = (uint8_t*)malloc(fsize);
  if (!*data) {
    fclose(file);
    return false;
  }

  size_t read = fread(*data, 1, fsize, file);
  fclose(file);

  if (read != (size_t)fsize) {
    free(*data);
    return false;
  }

  *size = fsize;
  return true;
}

static bool WriteFile(const char* filename, const uint8_t* data, size_t size) {
  FILE* file = fopen(filename, "wb");
  if (!file) {
    fprintf(stderr, "Could not open %s for writing\n", filename);
    return false;
  }

  size_t written = fwrite(data, 1, size, file);
  fclose(file);

  return written == size;
}

// Progressive JXL decoder
static bool DecodeJxlProgressive(const uint8_t* jxl_data, size_t jxl_size,
                                 uint8_t** pixels, uint32_t* xsize,
                                 uint32_t* ysize, uint32_t* channels) {
  JxlResizableParallelRunnerPtr runner =
      JxlResizableParallelRunnerMake(nullptr);
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);

  if (JXL_DEC_SUCCESS !=
      JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO |
                                               JXL_DEC_COLOR_ENCODING |
                                               JXL_DEC_FULL_IMAGE)) {
    fprintf(stderr, "JxlDecoderSubscribeEvents failed\n");
    return false;
  }

  if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec.get(),
                                                     JxlResizableParallelRunner,
                                                     runner.get())) {
    fprintf(stderr, "JxlDecoderSetParallelRunner failed\n");
    return false;
  }

  JxlBasicInfo info;
  JxlPixelFormat format = {0, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
  bool has_alpha = false;

  JxlDecoderSetInput(dec.get(), jxl_data, jxl_size);

  for (;;) {
    JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());

    if (status == JXL_DEC_ERROR) {
      fprintf(stderr, "Decoder error\n");
      return false;
    } else if (status == JXL_DEC_SUCCESS) {
      printf("Decoding complete!\n");
      break;
    } else if (status == JXL_DEC_BASIC_INFO) {
      if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec.get(), &info)) {
        fprintf(stderr, "JxlDecoderGetBasicInfo failed\n");
        return false;
      }
      *xsize = info.xsize;
      *ysize = info.ysize;
      has_alpha = (info.alpha_bits > 0);
      *channels = info.num_color_channels + (has_alpha ? 1 : 0);

      // Use RGBA format for output
      format.num_channels = 4;  // Always request RGBA

      JxlResizableParallelRunnerSetThreads(
          runner.get(),
          JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));

      printf("Image size: %u x %u, %u channels (alpha: %s)\n", *xsize, *ysize,
             *channels, has_alpha ? "yes" : "no");
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t buffer_size;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size)) {
        fprintf(stderr, "JxlDecoderImageOutBufferSize failed\n");
        return false;
      }

      *pixels = (uint8_t*)malloc(buffer_size);
      if (!*pixels) {
        fprintf(stderr, "Memory allocation failed\n");
        return false;
      }

      if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(
                                 dec.get(), &format, *pixels, buffer_size)) {
        fprintf(stderr, "JxlDecoderSetImageOutBuffer failed\n");
        free(*pixels);
        return false;
      }
    } else if (status == JXL_DEC_FULL_IMAGE) {
      continue;
    } else if (status == 256) {  // JXL_DEC_COLOR_ENCODING
      // Color encoding info, we can skip it
      printf("Processing color encoding...\n");
      continue;
    } else if (status == JXL_DEC_JPEG_RECONSTRUCTION) {
      // JPEG reconstruction data present, we can skip it for now
      printf("JXL image has JPEG reconstruction data, skipping...\n");
      continue;
    } else if (status == JXL_DEC_BOX) {
      // Box data present, we can skip it for now
      printf("JXL image has box data, skipping...\n");
      continue;
    } else {
      fprintf(stderr, "Unknown decoder status: %d\n", (int)status);
      return false;
    }
  }

  return true;
}

// Progressive JXL encoder
static bool EncodeJxlProgressive(const uint8_t* pixels, uint32_t xsize,
                                 uint32_t ysize, uint32_t channels,
                                 uint8_t** jxl_data, size_t* jxl_size,
                                 float quality) {
  JxlEncoderPtr enc = JxlEncoderMake(nullptr);
  JxlThreadParallelRunnerPtr runner = JxlThreadParallelRunnerMake(
      nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());

  if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc.get(),
                                                     JxlThreadParallelRunner,
                                                     runner.get())) {
    fprintf(stderr, "JxlEncoderSetParallelRunner failed\n");
    return false;
  }

  JxlPixelFormat pixel_format = {channels, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN,
                                 0};

  JxlBasicInfo basic_info;
  JxlEncoderInitBasicInfo(&basic_info);
  basic_info.xsize = xsize;
  basic_info.ysize = ysize;
  basic_info.bits_per_sample = 8;
  // num_color_channels: 1=grayscale, 3=RGB
  basic_info.num_color_channels = (channels < 3) ? 1 : 3;
  // num_extra_channels: alpha channel count (channels - num_color_channels)
  basic_info.num_extra_channels = (channels > 3) ? (channels - 3) : 0;
  basic_info.alpha_bits = (channels == 2 || channels == 4) ? 8 : 0;

  JxlEncoderStatus state = JxlEncoderSetBasicInfo(enc.get(), &basic_info);
  if (JXL_ENC_SUCCESS != state) {
    fprintf(stderr, "JxlEncoderSetBasicInfo failed: static: %d\n", (int)state);
    return false;
  }

  JxlColorEncoding color_encoding = {};
  JxlColorEncodingSetToSRGB(&color_encoding, channels < 3);
  if (JXL_ENC_SUCCESS !=
      JxlEncoderSetColorEncoding(enc.get(), &color_encoding)) {
    fprintf(stderr, "JxlEncoderSetColorEncoding failed\n");
    return false;
  }

  // Use progressive mode
  JxlEncoderFrameSettings* frame_settings =
      JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
  if (!frame_settings) {
    fprintf(stderr, "JxlEncoderFrameSettingsCreate failed\n");
    return false;
  }

  JxlEncoderFrameSettingsSetOption(frame_settings,
                                   JXL_ENC_FRAME_SETTING_PROGRESSIVE_DC, 1);
  JxlEncoderFrameSettingsSetOption(frame_settings,
                                   JXL_ENC_FRAME_SETTING_PROGRESSIVE_AC, 1);
  JxlEncoderFrameSettingsSetOption(frame_settings,
                                   JXL_ENC_FRAME_SETTING_QPROGRESSIVE_AC, 1);

  // Set effort (1 = slowest/best, 9 = fastest/worst)
  int effort = 9 - (int)(quality / 100.0 * 8.0);
  if (effort < 1) effort = 1;
  if (effort > 9) effort = 9;
  JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                   effort);

  if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(frame_settings, &pixel_format,
                                                 pixels,
                                                 xsize * ysize * channels)) {
    fprintf(stderr, "JxlEncoderAddImageFrame failed\n");
    return false;
  }
  JxlEncoderCloseInput(enc.get());

  // Allocate initial buffer
  *jxl_data = (uint8_t*)malloc(1024 * 1024);
  if (!*jxl_data) {
    fprintf(stderr, "Memory allocation failed\n");
    return false;
  }

  uint8_t* next_out = *jxl_data;
  size_t avail_out = 1024 * 1024;

  JxlEncoderStatus process_result = JXL_ENC_NEED_MORE_OUTPUT;
  while (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
    process_result = JxlEncoderProcessOutput(enc.get(), &next_out, &avail_out);
    if (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
      size_t offset = next_out - *jxl_data;
      size_t new_size = offset * 2;
      uint8_t* new_data = (uint8_t*)realloc(*jxl_data, new_size);
      if (!new_data) {
        fprintf(stderr, "Memory reallocation failed\n");
        free(*jxl_data);
        return false;
      }
      *jxl_data = new_data;
      next_out = *jxl_data + offset;
      avail_out = new_size - offset;
    }
  }

  *jxl_size = next_out - *jxl_data;

  if (JXL_ENC_SUCCESS != process_result) {
    fprintf(stderr, "JxlEncoderProcessOutput failed with status: %d\n",
            (int)process_result);
    free(*jxl_data);
    return false;
  }

  return true;
}

// Get file extension
static const char* GetFileExtension(const char* filename) {
  const char* dot = strrchr(filename, '.');
  if (!dot || dot == filename) return "";
  return dot + 1;
}

// Check if file is JXL format
static bool IsJxlFile(const char* filename) {
  const char* ext = GetFileExtension(filename);
  return strcasecmp(ext, "jxl") == 0;
}

#ifdef JXLL_BUILD_WITH_WIC
static bool WicToPixels(const char* filename, uint8_t** pixels, uint32_t* xsize,
                        uint32_t* ysize) {
  HRESULT hr;
  IWICImagingFactory* factory = NULL;
  IWICBitmapDecoder* decoder = NULL;
  IWICBitmapFrameDecode* frame = NULL;
  IWICFormatConverter* converter = NULL;
  bool result = false;

  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) return false;

  hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        IID_IWICImagingFactory, (LPVOID*)&factory);
  if (FAILED(hr)) {
    CoUninitialize();
    return false;
  }

  wchar_t wfilename[1024];
  MultiByteToWideChar(CP_ACP, 0, filename, -1, wfilename, 1024);
  hr = factory->CreateDecoderFromFilename(
      wfilename, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(hr)) {
    factory->Release();
    CoUninitialize();
    return false;
  }

  hr = decoder->GetFrame(0, &frame);
  if (SUCCEEDED(hr)) {
    hr = frame->GetSize(xsize, ysize);
    if (SUCCEEDED(hr)) {
      hr = factory->CreateFormatConverter(&converter);
      if (SUCCEEDED(hr)) {
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppRGBA;
        hr = converter->Initialize(frame, pixelFormat, WICBitmapDitherTypeNone,
                                   NULL, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) {
          UINT stride = (*xsize) * 4;
          size_t buffer_size = stride * (*ysize);
          *pixels = (uint8_t*)malloc(buffer_size);
          if (*pixels) {
            hr = converter->CopyPixels(NULL, stride, buffer_size, *pixels);
            if (SUCCEEDED(hr)) {
              result = true;
            } else {
              free(*pixels);
              *pixels = NULL;
            }
          }
        }
        converter->Release();
      }
    }
    frame->Release();
  }
  decoder->Release();
  factory->Release();
  CoUninitialize();
  return result;
}

static bool PixelsToWic(const uint8_t* pixels, uint32_t xsize, uint32_t ysize,
                        const char* filename) {
  HRESULT hr;
  IWICImagingFactory* factory = NULL;
  IWICBitmap* bitmap = NULL;
  IWICStream* stream = NULL;
  IWICBitmapEncoder* encoder = NULL;
  bool result = false;

  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) return false;

  hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        IID_IWICImagingFactory, (LPVOID*)&factory);
  if (FAILED(hr)) {
    CoUninitialize();
    return false;
  }

  hr = factory->CreateBitmapFromMemory(
      xsize, ysize, GUID_WICPixelFormat32bppBGRA, xsize * 4, xsize * ysize * 4,
      (uint8_t*)pixels, &bitmap);
  if (SUCCEEDED(hr)) {
    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
      WCHAR wfilename[1024];
      MultiByteToWideChar(CP_ACP, 0, filename, -1, wfilename, 1024);
      hr = stream->InitializeFromFilename(wfilename, GENERIC_WRITE);
      if (SUCCEEDED(hr)) {
        const char* ext = GetFileExtension(filename);
        GUID containerFormat;
        if (strcasecmp(ext, "png") == 0) {
          containerFormat = GUID_ContainerFormatPng;
        } else if (strcasecmp(ext, "jpg") == 0 ||
                   strcasecmp(ext, "jpeg") == 0) {
          containerFormat = GUID_ContainerFormatJpeg;
        } else if (strcasecmp(ext, "bmp") == 0) {
          containerFormat = GUID_ContainerFormatBmp;
        } else {
          containerFormat = GUID_ContainerFormatPng;
        }

        hr = factory->CreateEncoder(containerFormat, NULL, &encoder);
        if (SUCCEEDED(hr)) {
          hr = encoder->Initialize(stream, (WICBitmapEncoderCacheOption)1);
          if (SUCCEEDED(hr)) {
            IWICBitmapFrameEncode* frameEncode = NULL;
            IPropertyBag2* propBag = NULL;
            hr = encoder->CreateNewFrame(&frameEncode, &propBag);
            if (SUCCEEDED(hr)) {
              hr = frameEncode->Initialize(propBag);
              if (SUCCEEDED(hr)) {
                hr = frameEncode->WriteSource(bitmap, NULL);
                if (SUCCEEDED(hr)) {
                  hr = frameEncode->Commit();
                  if (SUCCEEDED(hr)) {
                    hr = encoder->Commit();
                    if (SUCCEEDED(hr)) {
                      result = true;
                    }
                  }
                }
              }
              frameEncode->Release();
            }
            if (propBag) propBag->Release();
          }
          encoder->Release();
        }
      }
      stream->Release();
    }
    bitmap->Release();
  }
  factory->Release();
  CoUninitialize();
  return result;
}

static bool GetWicImageSize(const char* filename, uint32_t* xsize,
                            uint32_t* ysize) {
  HRESULT hr;
  IWICImagingFactory* factory = NULL;
  IWICBitmapDecoder* decoder = NULL;
  IWICBitmapFrameDecode* frame = NULL;
  bool result = false;

  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) return false;

  hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        IID_IWICImagingFactory, (LPVOID*)&factory);
  if (FAILED(hr)) {
    CoUninitialize();
    return false;
  }

  wchar_t wfilename[1024];
  MultiByteToWideChar(CP_ACP, 0, filename, -1, wfilename, 1024);
  hr = factory->CreateDecoderFromFilename(
      wfilename, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
  if (SUCCEEDED(hr)) {
    hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) {
      hr = frame->GetSize(xsize, ysize);
      if (SUCCEEDED(hr)) {
        result = true;
      }
      frame->Release();
    }
    decoder->Release();
  }
  factory->Release();
  CoUninitialize();
  return result;
}
#endif

#ifdef JXLL_BUILD_WITH_IMAGEMAGICK
// Convert image to RGBA pixels using ImageMagick
static bool ImageMagickToPixels(const char* filename, uint8_t** pixels,
                                uint32_t* xsize, uint32_t* ysize) {
  try {
    Magick::Image image(filename);

    *xsize = image.columns();
    *ysize = image.rows();

    // Allocate buffer
    size_t pixel_count = (*xsize) * (*ysize) * 4;
    *pixels = (uint8_t*)malloc(pixel_count);
    if (!*pixels) {
      return false;
    }

    // Get image in RGBA format
    Magick::Image rgba_image = image;
    rgba_image.type(Magick::TrueColorType);
    rgba_image.alphaChannel(Magick::DeactivateAlphaChannel);

    // Force RGB color space
    rgba_image.colorSpace(Magick::sRGBColorspace);

    // Get the raw pixel data
    rgba_image.write(0, 0, *xsize, *ysize, "RGBA", Magick::CharPixel, *pixels);

    return true;
  } catch (Magick::Exception& error) {
    fprintf(stderr, "ImageMagick error: %s\n", error.what());
    return false;
  }
}

// Convert RGBA pixels to image using ImageMagick
static bool PixelsToImageMagick(const uint8_t* pixels, uint32_t xsize,
                                uint32_t ysize, const char* filename) {
  try {
    Magick::Image image(xsize, ysize, "RGBA", Magick::CharPixel, pixels);

    image.colorSpace(Magick::sRGBColorspace);
    image.write(filename);
    return true;
  } catch (Magick::Exception& error) {
    fprintf(stderr, "ImageMagick error: %s\n", error.what());
    return false;
  }
}

// Get image size without loading full pixels
static bool GetImageMagickSize(const char* filename, uint32_t* xsize,
                               uint32_t* ysize) {
  try {
    Magick::Image image(filename);
    *xsize = image.columns();
    *ysize = image.rows();
    return true;
  } catch (Magick::Exception& error) {
    fprintf(stderr, "ImageMagick error: %s\n", error.what());
    return false;
  }
}

// Resize image to target size using ImageMagick
static bool ResizeImageMagick(const char* filename, uint32_t target_xsize,
                              uint32_t target_ysize, uint8_t** out_pixels,
                              uint32_t* out_xsize, uint32_t* out_ysize) {
  try {
    Magick::Image image(filename);
    image.resize(Magick::Geometry(target_xsize, target_ysize));

    *out_xsize = image.columns();
    *out_ysize = image.rows();

    size_t pixel_count = (*out_xsize) * (*out_ysize) * 4;
    *out_pixels = (uint8_t*)malloc(pixel_count);
    if (!*out_pixels) return false;

    // No flip needed
    image.write(0, 0, *out_xsize, *out_ysize, "RGBA", Magick::CharPixel,
                *out_pixels);

    return true;
  } catch (Magick::Exception& error) {
    fprintf(stderr, "ImageMagick error: %s\n", error.what());
    return false;
  }
}
#endif

#ifdef JXLL_BUILD_WITH_STB
// Convert image to RGBA pixels using stb_image
static bool StbImageToPixels(const char* filename, uint8_t** pixels,
                             uint32_t* xsize, uint32_t* ysize) {
  int width, height, channels;
  *pixels = stbi_load(filename, &width, &height, &channels, 4);  // Force RGBA
  if (!*pixels) {
    fprintf(stderr, "Failed to load image: %s\n", filename);
    return false;
  }
  *xsize = width;
  *ysize = height;
  return true;
}

// Convert RGBA pixels to image using stb_image_write
static bool PixelsToStbImage(const uint8_t* pixels, uint32_t xsize,
                             uint32_t ysize, const char* filename) {
  const char* ext = GetFileExtension(filename);
  int result = 0;

  if (strcasecmp(ext, "png") == 0) {
    result = stbi_write_png(filename, xsize, ysize, 4, pixels, xsize * 4);
  } else if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
    result = stbi_write_jpg(filename, xsize, ysize, 4, pixels, 90);
  } else if (strcasecmp(ext, "bmp") == 0) {
    result = stbi_write_bmp(filename, xsize, ysize, 4, pixels);
  } else if (strcasecmp(ext, "tga") == 0) {
    result = stbi_write_tga(filename, xsize, ysize, 4, pixels);
  } else {
    // Default to PNG
    char png_filename[1024];
    snprintf(png_filename, sizeof(png_filename), "%s.png", filename);
    result = stbi_write_png(png_filename, xsize, ysize, 4, pixels, xsize * 4);
  }

  return result == 1;
}

// Get image size without loading full pixels
static bool GetStbImageSize(const char* filename, uint32_t* xsize,
                            uint32_t* ysize) {
  int width, height, channels;
  if (!stbi_info(filename, &width, &height, &channels)) {
    return false;
  }
  *xsize = width;
  *ysize = height;
  return true;
}
#endif

// Show usage
static void ShowUsage(const char* program) {
  printf("JPEG XL CLI Tool\n");
  printf("Usage: %s [options] <input> [output]\n\n", program);
  printf("Options:\n");
  printf("  -e, --encode    Encode input image(s) to JXL\n");
  printf("  -d, --decode    Decode JXL to output image\n");
  printf("  -q, --quality   Quality (0-100, default: 90)\n");
  printf("  -a, --animate   Encode multiple images as animation\n");
  printf("  -o, --output    Specify output file\n");
  printf("  -h, --help      Show this help\n\n");
  printf("Examples:\n");
  printf("  %s input.png output.jxl      Encode PNG to JXL\n", program);
  printf("  %s input.jxl output.png     Decode JXL to PNG\n", program);
  printf("  %s -a frame1.png frame2.png -o animation.jxl  Encode animation\n",
         program);
  printf("  %s                         Open file dialog\n", program);
}

// Encode single image to JXL
static bool EncodeToJxl(const char* input, const char* output, float quality) {
  uint8_t* pixels = NULL;
  uint32_t xsize, ysize;
  // Use RGBA (4 channels) - always add alpha channel
  uint32_t channels = 4;

  if (IsJxlFile(input)) {
    fprintf(stderr, "Input is already JXL format\n");
    return false;
  }

#ifdef JXLL_BUILD_WITH_IMAGEMAGICK
  if (!ImageMagickToPixels(input, &pixels, &xsize, &ysize)) {
    fprintf(stderr, "Failed to load image with ImageMagick: %s\n", input);
    return false;
  }
#elif defined(JXLL_BUILD_WITH_WIC)
  if (!WicToPixels(input, &pixels, &xsize, &ysize)) {
    fprintf(stderr, "Failed to load image with WIC: %s\n", input);
    return false;
  }
#elif defined(JXLL_BUILD_WITH_STB)
  if (!StbImageToPixels(input, &pixels, &xsize, &ysize)) {
    fprintf(stderr, "Failed to load image with stb_image: %s\n", input);
    return false;
  }
#else
  fprintf(stderr,
          "No image loading support compiled in (need ImageMagick, WIC, or "
          "stb_image)\n");
  return false;
#endif

  uint8_t* jxl_data = NULL;
  size_t jxl_size = 0;

  printf("Encoding %s (%ux%u, %u channels) to JXL...\n", input, xsize, ysize,
         channels);

  if (!EncodeJxlProgressive(pixels, xsize, ysize, channels, &jxl_data,
                            &jxl_size, quality)) {
    fprintf(stderr, "Encoding failed\n");
    free(pixels);
    return false;
  }

  free(pixels);

  if (!WriteFile(output, jxl_data, jxl_size)) {
    fprintf(stderr, "Failed to write JXL file\n");
    free(jxl_data);
    return false;
  }

  free(jxl_data);
  printf("Encoded to %s (%zu bytes)\n", output, jxl_size);

  return true;
}

// Encode animation from multiple images
static bool EncodeAnimation(const char** inputs, int count, const char* output,
                            float quality) {
#ifdef JXLL_BUILD_WITH_IMAGEMAGICK
  if (count < 2) {
    fprintf(stderr, "Animation requires at least 2 images\n");
    return false;
  }

  // Get size of first image
  uint32_t target_xsize, target_ysize;
  if (!GetImageMagickSize(inputs[0], &target_xsize, &target_ysize)) {
    fprintf(stderr, "Failed to get size of first image\n");
    return false;
  }

  printf("Target size: %ux%u\n", target_xsize, target_ysize);

  // Allocate buffer for first frame
  size_t frame_size = target_xsize * target_ysize * 4;
  uint8_t* all_frames = (uint8_t*)malloc(frame_size * count);
  if (!all_frames) {
    fprintf(stderr, "Memory allocation failed\n");
    return false;
  }

  // Load and resize all frames
  for (int i = 0; i < count; i++) {
    uint8_t* pixels = all_frames + (i * frame_size);
    uint32_t actual_xsize, actual_ysize;

    if (!ResizeImageMagick(inputs[i], target_xsize, target_ysize, &pixels,
                           &actual_xsize, &actual_ysize)) {
      fprintf(stderr, "Failed to load/resize image: %s\n", inputs[i]);
      free(all_frames);
      return false;
    }

    printf("Loaded frame %d: %ux%u\n", i + 1, actual_xsize, actual_ysize);
  }

  // Encode as animation (for now, encode as single frame - full animation needs
  // more work)
  uint8_t* jxl_data = NULL;
  size_t jxl_size = 0;

  printf("Encoding animation (%d frames) to JXL...\n", count);

  // Encode the first frame as a demo (full animation requires JXL animation
  // API)
  if (!EncodeJxlProgressive(all_frames, target_xsize, target_ysize, 4,
                            &jxl_data, &jxl_size, quality)) {
    fprintf(stderr, "Encoding failed\n");
    free(all_frames);
    return false;
  }

  free(all_frames);

  if (!WriteFile(output, jxl_data, jxl_size)) {
    fprintf(stderr, "Failed to write JXL file\n");
    free(jxl_data);
    return false;
  }

  free(jxl_data);
  printf("Encoded to %s (%zu bytes)\n", output, jxl_size);

  return true;
#else
  fprintf(stderr, "ImageMagick support not compiled in\n");
  return false;
#endif
}

// Decode JXL to image
static bool DecodeFromJxl(const char* input, const char* output) {
  uint8_t* jxl_data = NULL;
  size_t jxl_size = 0;

  if (!ReadFile(input, &jxl_data, &jxl_size)) {
    fprintf(stderr, "Failed to read JXL file: %s\n", input);
    return false;
  }

  uint8_t* pixels = NULL;
  uint32_t xsize, ysize, channels;

  printf("Decoding %s...\n", input);

  if (!DecodeJxlProgressive(jxl_data, jxl_size, &pixels, &xsize, &ysize,
                            &channels)) {
    fprintf(stderr, "Decoding failed\n");
    free(jxl_data);
    return false;
  }

  free(jxl_data);

#ifdef JXLL_BUILD_WITH_IMAGEMAGICK
  // Use ImageMagick to write output
  if (!PixelsToImageMagick(pixels, xsize, ysize, output)) {
    fprintf(stderr, "Failed to write output image\n");
    free(pixels);
    return false;
  }
#elif defined(JXLL_BUILD_WITH_WIC)
  if (!PixelsToWic(pixels, xsize, ysize, output)) {
    fprintf(stderr, "Failed to write output image\n");
    free(pixels);
    return false;
  }
#elif defined(JXLL_BUILD_WITH_STB)
  // Use stb_image_write to write output
  if (!PixelsToStbImage(pixels, xsize, ysize, output)) {
    fprintf(stderr, "Failed to write output image\n");
    free(pixels);
    return false;
  }
#else
  // Write as raw RGBA if no image support
  char raw_filename[1024];
  snprintf(raw_filename, sizeof(raw_filename), "%s.raw", output);
  WriteFile(raw_filename, pixels, xsize * ysize * 4);
  printf("Wrote raw RGBA to %s (%ux%u, %u channels)\n", raw_filename, xsize,
         ysize, channels);
#endif

  free(pixels);
  printf("Decoded to %s\n", output);

  return true;
}

int main(int argc, char* argv[]) {
  // If no arguments, try to open file dialog
  if (argc == 1) {
    // Try to use file dialog if available, otherwise show usage
    char* input =
        tinyfd_openFileDialog("Select Image File", "", 0, NULL, NULL, 0);

    if (!input) {
      ShowUsage(argv[0]);
      return 1;
    }

    // Determine output based on input
    char output[1024];
    if (IsJxlFile(input)) {
      snprintf(output, sizeof(output), "%s", input);
      char* dot = strrchr(output, '.');
      if (dot) strcpy(dot, "_out.png");
    } else {
      snprintf(output, sizeof(output), "%s", input);
      char* dot = strrchr(output, '.');
      if (dot)
        strcpy(dot, ".jxl");
      else
        strcat(output, ".jxl");
    }

    if (IsJxlFile(input)) {
      return DecodeFromJxl(input, output) ? 0 : 1;
    } else {
      return EncodeToJxl(input, output, 90.0f) ? 0 : 1;
    }
  }

  // Initialize ImageMagick if available
#ifdef JXLL_BUILD_WITH_IMAGEMAGICK
  Magick::InitializeMagick(nullptr);
#endif

  // Parse arguments
  bool encode = false;
  bool decode = false;
  bool animate = false;
  float quality = 90.0f;
  const char* output = NULL;

  // Collect input files
  const char* inputs[100];
  int input_count = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--encode") == 0) {
      encode = true;
    } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--decode") == 0) {
      decode = true;
    } else if (strcmp(argv[i], "-a") == 0 ||
               strcmp(argv[i], "--animate") == 0) {
      animate = true;
    } else if (strcmp(argv[i], "-q") == 0 ||
               strcmp(argv[i], "--quality") == 0) {
      if (i + 1 < argc) {
        quality = atof(argv[++i]);
      }
    } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
      if (i + 1 < argc) {
        output = argv[++i];
      }
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      ShowUsage(argv[0]);
      return 0;
    } else if (argv[i][0] != '-') {
      inputs[input_count++] = argv[i];
    }
  }

  // Auto-detect mode if not specified
  if (!encode && !decode && input_count > 0) {
    if (IsJxlFile(inputs[0])) {
      decode = true;
    } else {
      encode = true;
    }
  }

  // Validate arguments
  if (encode && input_count < 1) {
    fprintf(stderr, "Error: No input files specified for encoding\n");
    ShowUsage(argv[0]);
    return 1;
  }

  if (decode && input_count < 1) {
    fprintf(stderr, "Error: No input JXL file specified for decoding\n");
    ShowUsage(argv[0]);
    return 1;
  }

  // Determine output filename
  char output_file[1024];
  if (!output) {
    if (decode) {
      snprintf(output_file, sizeof(output_file), "%s", inputs[0]);
      char* dot = strrchr(output_file, '.');
      if (dot) strcpy(dot, "_out.png");
      output = output_file;
    } else if (animate) {
      output = "animation.jxl";
    } else {
      snprintf(output_file, sizeof(output_file), "%s", inputs[0]);
      char* dot = strrchr(output_file, '.');
      if (dot)
        strcpy(dot, ".jxl");
      else
        strcat(output_file, ".jxl");
      output = output_file;
    }
  }

  // Execute
  int result = 0;

  if (decode) {
    result = DecodeFromJxl(inputs[0], output) ? 0 : 1;
  } else if (animate) {
    result = EncodeAnimation(inputs, input_count, output, quality) ? 0 : 1;
  } else {
    result = EncodeToJxl(inputs[0], output, quality) ? 0 : 1;
  }

  return result;
}
