// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifdef JXLL_USE_IMAGEMAGICK
#include <Magick++.h>
#else
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#endif
#include <gtkmm.h>

#ifdef JXLL_USE_WIC
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <shcore.h>
#include <wincodec.h>
#include <windows.h>
#endif
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr size_t kInitialEncodeBufferSize = 1024 * 1024;
constexpr int kEffortFastest = 9;
constexpr int kEffortBalanced = 7;
constexpr float kQualityMax = 100.0f;
constexpr float kDistanceMax = 9.0f;

bool ReadFile(const char* filename, std::vector<uint8_t>* data) {
  FILE* file = fopen(filename, "rb");
  if (!file) return false;
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
  data->resize(static_cast<size_t>(fsize));
  const size_t read = fread(data->data(), 1, data->size(), file);
  fclose(file);
  return read == data->size();
}

bool WriteFile(const char* filename, const uint8_t* data, size_t size) {
  FILE* file = fopen(filename, "wb");
  if (!file) return false;
  const size_t written = fwrite(data, 1, size, file);
  fclose(file);
  return written == size;
}

const char* GetFileExtension(const char* filename) {
  const char* dot = strrchr(filename, '.');
  if (!dot || dot == filename) return "";
  return dot + 1;
}

bool IsJxlFile(const char* filename) {
  const char* ext = GetFileExtension(filename);
  return strcasecmp(ext, "jxl") == 0;
}

std::string DefaultOutput(const std::string& input, bool decode) {
  std::string output = input;
  const auto dot = output.find_last_of('.');
  if (decode) {
    if (dot != std::string::npos) output.resize(dot);
    output += "_out.png";
  } else {
    if (dot != std::string::npos) output.resize(dot);
    output += ".jxl";
  }
  return output;
}

#ifdef JXLL_USE_IMAGEMAGICK
bool ImageMagickToPixels(const char* filename, int channels,
                         std::vector<uint8_t>* pixels, uint32_t* xsize,
                         uint32_t* ysize) {
  try {
    Magick::Image image(filename);
    image.colorSpace(Magick::sRGBColorspace);
    *xsize = image.columns();
    *ysize = image.rows();
    const size_t size = static_cast<size_t>(*xsize) * (*ysize) * channels;
    pixels->resize(size);
    if (channels == 4) {
      image.alphaChannel(Magick::ActivateAlphaChannel);
      image.write(0, 0, *xsize, *ysize, "RGBA", Magick::CharPixel,
                  pixels->data());
    } else {
      image.alphaChannel(Magick::DeactivateAlphaChannel);
      image.write(0, 0, *xsize, *ysize, "RGB", Magick::CharPixel,
                  pixels->data());
    }
    return true;
  } catch (const Magick::Exception& error) {
    fprintf(stderr, "ImageMagick error: %s\n", error.what());
    return false;
  }
}

bool PixelsToImageMagick(const uint8_t* pixels, uint32_t xsize, uint32_t ysize,
                         int channels, const char* filename) {
  try {
    const char* map = channels == 4 ? "RGBA" : "RGB";
    Magick::Image image(xsize, ysize, map, Magick::CharPixel, pixels);
    image.colorSpace(Magick::sRGBColorspace);
    image.write(filename);
    return true;
  } catch (const Magick::Exception& error) {
    fprintf(stderr, "ImageMagick error: %s\n", error.what());
    return false;
  }
}
#elif defined(JXLL_USE_WIC)
bool WicToPixels(const char* filename, int channels,
                 std::vector<uint8_t>* pixels, uint32_t* xsize,
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

  WCHAR wfilename[1024];
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
          pixels->resize(buffer_size);
          hr = converter->CopyPixels(NULL, stride, buffer_size, pixels->data());
          if (SUCCEEDED(hr)) {
            result = true;
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

bool PixelsToWic(const uint8_t* pixels, uint32_t xsize, uint32_t ysize,
                 int channels, const char* filename) {
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

inline bool ImageMagickToPixels(const char* filename, int channels,
                                std::vector<uint8_t>* pixels, uint32_t* xsize,
                                uint32_t* ysize) {
  return WicToPixels(filename, channels, pixels, xsize, ysize);
}

inline bool PixelsToImageMagick(const uint8_t* pixels, uint32_t xsize,
                                uint32_t ysize, int channels,
                                const char* filename) {
  return PixelsToWic(pixels, xsize, ysize, channels, filename);
}
#else
// stb_image implementation
bool StbImageToPixels(const char* filename, int channels,
                      std::vector<uint8_t>* pixels, uint32_t* xsize,
                      uint32_t* ysize) {
  int width, height, img_channels;
  uint8_t* data = stbi_load(filename, &width, &height, &img_channels, channels);
  if (!data) {
    fprintf(stderr, "Failed to load image: %s\n", filename);
    return false;
  }
  *xsize = static_cast<uint32_t>(width);
  *ysize = static_cast<uint32_t>(height);
  const size_t size = static_cast<size_t>(*xsize) * (*ysize) * channels;
  pixels->assign(data, data + size);
  stbi_image_free(data);
  return true;
}

bool PixelsToStbImage(const uint8_t* pixels, uint32_t xsize, uint32_t ysize,
                      int channels, const char* filename) {
  const char* ext = GetFileExtension(filename);
  int result = 0;

  if (strcasecmp(ext, "png") == 0) {
    result = stbi_write_png(filename, xsize, ysize, channels, pixels,
                            xsize * channels);
  } else if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
    result = stbi_write_jpg(filename, xsize, ysize, channels, pixels, 90);
  } else if (strcasecmp(ext, "bmp") == 0) {
    result = stbi_write_bmp(filename, xsize, ysize, channels, pixels);
  } else if (strcasecmp(ext, "tga") == 0) {
    result = stbi_write_tga(filename, xsize, ysize, channels, pixels);
  } else {
    // Default to PNG
    std::string png_file = std::string(filename) + ".png";
    result = stbi_write_png(png_file.c_str(), xsize, ysize, channels, pixels,
                            xsize * channels);
  }

  return result == 1;
}

// Alias stb_image functions to the names used in the code
inline bool ImageMagickToPixels(const char* filename, int channels,
                                std::vector<uint8_t>* pixels, uint32_t* xsize,
                                uint32_t* ysize) {
  return StbImageToPixels(filename, channels, pixels, xsize, ysize);
}

inline bool PixelsToImageMagick(const uint8_t* pixels, uint32_t xsize,
                                uint32_t ysize, int channels,
                                const char* filename) {
  return PixelsToStbImage(pixels, xsize, ysize, channels, filename);
}
#endif

bool DecodeJxl(const std::vector<uint8_t>& jxl_data, int out_channels,
               std::vector<uint8_t>* pixels, uint32_t* xsize, uint32_t* ysize) {
  JxlResizableParallelRunnerPtr runner =
      JxlResizableParallelRunnerMake(nullptr);
  JxlDecoderPtr dec = JxlDecoderMake(nullptr);
  if (JXL_DEC_SUCCESS !=
      JxlDecoderSubscribeEvents(dec.get(),
                                JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)) {
    return false;
  }
  if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(dec.get(),
                                                     JxlResizableParallelRunner,
                                                     runner.get())) {
    return false;
  }

  JxlPixelFormat format = {static_cast<uint32_t>(out_channels), JXL_TYPE_UINT8,
                           JXL_NATIVE_ENDIAN, 0};
  JxlDecoderSetInput(dec.get(), jxl_data.data(), jxl_data.size());
  for (;;) {
    const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
    if (status == JXL_DEC_ERROR) return false;
    if (status == JXL_DEC_SUCCESS) return true;
    if (status == JXL_DEC_BASIC_INFO) {
      JxlBasicInfo info;
      if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec.get(), &info))
        return false;
      *xsize = info.xsize;
      *ysize = info.ysize;
      JxlResizableParallelRunnerSetThreads(
          runner.get(),
          JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
    } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t buffer_size = 0;
      if (JXL_DEC_SUCCESS !=
          JxlDecoderImageOutBufferSize(dec.get(), &format, &buffer_size)) {
        return false;
      }
      pixels->resize(buffer_size);
      if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec.get(), &format,
                                                         pixels->data(),
                                                         pixels->size())) {
        return false;
      }
    }
  }
}

bool EncodeJxl(const uint8_t* pixels, uint32_t xsize, uint32_t ysize,
               int channels, float quality, std::vector<uint8_t>* jxl_data) {
  JxlEncoderPtr enc = JxlEncoderMake(nullptr);
  JxlThreadParallelRunnerPtr runner = JxlThreadParallelRunnerMake(
      nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
  if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc.get(),
                                                     JxlThreadParallelRunner,
                                                     runner.get())) {
    return false;
  }

  JxlPixelFormat format = {static_cast<uint32_t>(channels), JXL_TYPE_UINT8,
                           JXL_NATIVE_ENDIAN, 0};
  JxlBasicInfo info;
  JxlEncoderInitBasicInfo(&info);
  info.xsize = xsize;
  info.ysize = ysize;
  info.bits_per_sample = 8;
  info.num_color_channels = 3;
  info.num_extra_channels = channels == 4 ? 1 : 0;
  info.alpha_bits = channels == 4 ? 8 : 0;

  if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc.get(), &info)) return false;

  JxlColorEncoding color_encoding = {};
  JxlColorEncodingSetToSRGB(&color_encoding, JXL_FALSE);
  if (JXL_ENC_SUCCESS !=
      JxlEncoderSetColorEncoding(enc.get(), &color_encoding)) {
    return false;
  }

  JxlEncoderFrameSettings* frame_settings =
      JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
  const int effort = std::clamp(kEffortBalanced, 1, kEffortFastest);
  const float clamped_quality = std::clamp(quality, 1.0f, kQualityMax);
  const float distance =
      (kQualityMax - clamped_quality) / kQualityMax * kDistanceMax;
  JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                   effort);
  if (JXL_ENC_SUCCESS != JxlEncoderSetFrameDistance(frame_settings, distance)) {
    return false;
  }

  const size_t bytes = static_cast<size_t>(xsize) * ysize * channels;
  if (JXL_ENC_SUCCESS !=
      JxlEncoderAddImageFrame(frame_settings, &format, pixels, bytes)) {
    return false;
  }
  JxlEncoderCloseInput(enc.get());

  jxl_data->assign(kInitialEncodeBufferSize, 0);
  uint8_t* next_out = jxl_data->data();
  size_t avail_out = jxl_data->size();
  JxlEncoderStatus status = JXL_ENC_NEED_MORE_OUTPUT;
  while (status == JXL_ENC_NEED_MORE_OUTPUT) {
    status = JxlEncoderProcessOutput(enc.get(), &next_out, &avail_out);
    if (status == JXL_ENC_NEED_MORE_OUTPUT) {
      const size_t offset = static_cast<size_t>(next_out - jxl_data->data());
      jxl_data->resize(jxl_data->size() * 2);
      next_out = jxl_data->data() + offset;
      avail_out = jxl_data->size() - offset;
    }
  }
  if (status != JXL_ENC_SUCCESS) return false;
  jxl_data->resize(static_cast<size_t>(next_out - jxl_data->data()));
  return true;
}

class GuiWindow : public Gtk::Window {
 public:
  GuiWindow()
      : root_(Gtk::Orientation::VERTICAL, 8),
        open_button_("Open"),
        action_button_("Run"),
        quality_scale_(Gtk::Adjustment::create(90.0, 1.0, 100.0, 1.0),
                       Gtk::Orientation::HORIZONTAL) {
    set_title("libjxl-lite gui");
    set_default_size(800, 600);
    set_child(root_);
    root_.set_margin(12);

    mode_combo_.append("Auto");
    mode_combo_.append("Encode");
    mode_combo_.append("Decode");
    mode_combo_.set_active_text("Auto");

    format_combo_.append("RGB");
    format_combo_.append("RGBA");
    format_combo_.set_active_text("RGBA");

    quality_scale_.set_hexpand(true);
    quality_scale_.set_draw_value(true);

    auto* file_row =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    file_label_.set_text("No file selected");
    file_label_.set_hexpand(true);
    file_label_.set_halign(Gtk::Align::START);
    open_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &GuiWindow::OnOpenFile));
    file_row->append(file_label_);
    file_row->append(open_button_);
    root_.append(*file_row);

    preview_box_.set_size_request(400, 300);
    root_.append(preview_box_);

    AddRow("Output file", output_entry_);
    AddRow("Mode", mode_combo_);
    AddRow("Output format", format_combo_);
    AddRow("Quality", quality_scale_);

    action_button_.signal_clicked().connect(
        sigc::mem_fun(*this, &GuiWindow::OnRun));
    root_.append(action_button_);
    status_.set_halign(Gtk::Align::START);
    root_.append(status_);
  }

 private:
  template <typename Widget>
  void AddRow(const Glib::ustring& name, Widget& widget) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* label = Gtk::make_managed<Gtk::Label>(name);
    label->set_size_request(120, -1);
    label->set_halign(Gtk::Align::START);
    widget.set_hexpand(true);
    row->append(*label);
    row->append(widget);
    root_.append(*row);
  }

  void SetStatus(const std::string& text) { status_.set_text(text); }

  void OnOpenFile() {
    file_dialog_ = Gtk::FileDialog::create();
    file_dialog_->set_title("Select Image File");

    auto all_filter = Gtk::FileFilter::create();
    all_filter->set_name("All supported files");
    all_filter->add_pattern("*.jxl");
    all_filter->add_pattern("*.png");
    all_filter->add_pattern("*.jpg");
    all_filter->add_pattern("*.jpeg");
    all_filter->add_pattern("*.bmp");
    all_filter->add_pattern("*.tga");
    all_filter->add_pattern("*.gif");

    auto jxl_filter = Gtk::FileFilter::create();
    jxl_filter->set_name("JPEG XL files");
    jxl_filter->add_pattern("*.jxl");

    auto image_filter = Gtk::FileFilter::create();
    image_filter->set_name("Image files");
    image_filter->add_pattern("*.png");
    image_filter->add_pattern("*.jpg");
    image_filter->add_pattern("*.jpeg");
    image_filter->add_pattern("*.bmp");
    image_filter->add_pattern("*.tga");
    image_filter->add_pattern("*.gif");

    auto filter_list = Gio::ListStore<Gtk::FileFilter>::create();
    filter_list->append(all_filter);
    filter_list->append(jxl_filter);
    filter_list->append(image_filter);
    file_dialog_->set_filters(filter_list);

    file_dialog_->open(sigc::mem_fun(*this, &GuiWindow::OnFileSelected));
  }

  void OnFileSelected(Glib::RefPtr<Gio::AsyncResult> result) {
    Glib::RefPtr<Gio::File> file;
    try {
      file = file_dialog_->open_finish(result);
    } catch (...) {
      return;
    }
    if (!file) return;

    current_file_ = file->get_path();
    file_label_.set_text(current_file_);

    bool is_jxl = IsJxlFile(current_file_.c_str());
    if (is_jxl) {
      mode_combo_.set_active_text("Decode");
      SetStatus("JXL file detected - mode set to Decode");
    } else {
      mode_combo_.set_active_text("Encode");
      SetStatus("Image file detected - mode set to Encode");
    }

    UpdatePreview(current_file_);
  }

  void UpdatePreview(const std::string& filename) {
    for (auto child = preview_box_.get_first_child(); child != nullptr;) {
      auto next = child->get_next_sibling();
      preview_box_.remove(*child);
      child = next;
    }

    if (filename.empty()) return;

    bool is_jxl = IsJxlFile(filename.c_str());

    if (is_jxl) {
      std::vector<uint8_t> jxl_data;
      if (!ReadFile(filename.c_str(), &jxl_data)) {
        SetStatus("Failed to read JXL file for preview.");
        return;
      }

      std::vector<uint8_t> pixels;
      uint32_t xsize = 0, ysize = 0;
      if (!DecodeJxl(jxl_data, 4, &pixels, &xsize, &ysize)) {
        SetStatus("Failed to decode JXL for preview.");
        return;
      }

      std::string preview_file = filename + ".preview.png";
      if (PixelsToImageMagick(pixels.data(), xsize, ysize, 4,
                              preview_file.c_str())) {
        auto picture = Gtk::Picture();
        picture.set_filename(preview_file);
        picture.set_size_request(400, 300);
        picture.set_halign(Gtk::Align::CENTER);
        picture.set_valign(Gtk::Align::CENTER);
        preview_box_.append(picture);

        std::remove(preview_file.c_str());
      }
    } else {
      auto picture = Gtk::Picture();
      picture.set_filename(filename);
      picture.set_size_request(400, 300);
      picture.set_halign(Gtk::Align::CENTER);
      picture.set_valign(Gtk::Align::CENTER);
      preview_box_.append(picture);
    }
  }

  void OnRun() {
    const std::string input = current_file_;
    std::string output = output_entry_.get_text();
    if (input.empty()) {
      SetStatus("Please select a file first.");
      return;
    }

    bool decode = false;
    const std::string mode = mode_combo_.get_active_text();
    if (mode == "Decode") {
      decode = true;
    } else if (mode == "Encode") {
      decode = false;
    } else {
      decode = IsJxlFile(input.c_str());
    }
    if (output.empty()) output = DefaultOutput(input, decode);

    const int channels = format_combo_.get_active_text() == "RGB" ? 3 : 4;
    const float quality = static_cast<float>(quality_scale_.get_value());

    if (decode) {
      std::vector<uint8_t> jxl_data;
      if (!ReadFile(input.c_str(), &jxl_data)) {
        SetStatus("Failed to read input JXL.");
        return;
      }
      std::vector<uint8_t> pixels;
      uint32_t xsize = 0, ysize = 0;
      if (!DecodeJxl(jxl_data, channels, &pixels, &xsize, &ysize) ||
          !PixelsToImageMagick(pixels.data(), xsize, ysize, channels,
                               output.c_str())) {
        SetStatus("Decode failed.");
        return;
      }
      SetStatus("Decode success: " + output);
      UpdatePreview(output);
      return;
    }

    std::vector<uint8_t> pixels;
    uint32_t xsize = 0, ysize = 0;
    if (!ImageMagickToPixels(input.c_str(), channels, &pixels, &xsize,
                             &ysize)) {
      SetStatus("Failed to read input image.");
      return;
    }
    std::vector<uint8_t> jxl_data;
    if (!EncodeJxl(pixels.data(), xsize, ysize, channels, quality, &jxl_data) ||
        !WriteFile(output.c_str(), jxl_data.data(), jxl_data.size())) {
      SetStatus("Encode failed.");
      return;
    }
    SetStatus("Encode success: " + output);
  }

  Gtk::Box root_;
  Gtk::Label file_label_;
  Gtk::Button open_button_;
  Gtk::Box preview_box_;
  Gtk::Entry output_entry_;
  Gtk::ComboBoxText mode_combo_;
  Gtk::ComboBoxText format_combo_;
  Gtk::Button action_button_;
  Gtk::Scale quality_scale_;
  Gtk::Label status_;
  std::string current_file_;
  Glib::RefPtr<Gtk::FileDialog> file_dialog_;
};

}  // namespace

int main(int argc, char* argv[]) {
#ifdef JXLL_USE_IMAGEMAGICK
  Magick::InitializeMagick(argv[0]);
#endif
  auto app = Gtk::Application::create("org.libjxl.lite.gui");
  return app->make_window_and_run<GuiWindow>(argc, argv);
}
