// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
// #include <cmath>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "jxl/codestream_header.h"
#include "jxl/decode_cxx.h"
#include "jxl/encode_cxx.h"
#include "jxl/resizable_parallel_runner_cxx.h"
#include "jxl/thread_parallel_runner_cxx.h"

#define G_LOG_DOMAIN "App"

#include <exiv2/exiv2.hpp>
#include <gio/gio.h>
#include <gtkmm.h>
#define DEF_WINDOW_WIDTH 600
#define DEF_WINDOW_HEIGHT 400
#define DEF_ROW_HEIGHT 40
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <clocale>
#include <windows.h>
#ifdef JXLL_BUILD_WITH_WIC
#include <propvarutil.h>
#include <shcore.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif
#define sys_beep_success() MessageBeep(MB_OK)
#define sys_beep_error() MessageBeep(MB_ICONERROR)
#endif
#pragma region common
// RAII 包装器：自动释放 g_malloc 分配的内存

struct GBuffWrap {
    uint8_t* buff;
    size_t size;
    GBuffWrap(const GBuffWrap&) = delete;
    GBuffWrap& operator=(const GBuffWrap&) = delete;
    explicit GBuffWrap(char* b) noexcept
        : buff(reinterpret_cast<uint8_t*>(b))
    {
        size = strlen(b);
    }
    explicit GBuffWrap(uint8_t* b, size_t s) noexcept
        : buff(b)
        , size(s)
    {
    }
    GBuffWrap(GBuffWrap&& other) noexcept
        : buff(other.buff)
        , size(other.size)
    {
        other.buff = nullptr;
        other.size = 0;
    }
    GBuffWrap& operator=(GBuffWrap&& other) noexcept
    {
        if (this != &other) {
            g_clear_pointer(&buff, g_free);
            buff = other.buff;
            size = other.size;
            other.buff = nullptr;
            other.size = 0;
        }
        return *this;
    }
    ~GBuffWrap()
    {
        g_clear_pointer(&buff, g_free);
    }
};

struct GFileWrap : public GBuffWrap {
    GFile* file;
    GFileWrap(const GFileWrap&) = delete;
    GFileWrap& operator=(const GFileWrap&) = delete;
    explicit GFileWrap(char* p)
        : GBuffWrap(p)
    {
        file = g_file_new_for_path(p);
    }
    GFileWrap(GFileWrap&& other) noexcept
        : GBuffWrap(std::move(other))
        , file(other.file)
    {
        other.file = nullptr; // 将原对象置空，防止析构时销毁资源
    }
    GFileWrap& operator=(GFileWrap&& other) noexcept
    {
        if (this != &other) {
            GBuffWrap::operator=(std::move(other));
            g_clear_object(&file);
            file = other.file;
            other.file = nullptr;
        }
        return *this;
    }
    ~GFileWrap()
    {
        g_clear_object(&file);
    }
};

static std::shared_ptr<GBuffWrap> g_file_load_contents_wrap(GFile* file, GCancellable* cancellable)
{
    char* buff = NULL;
    gsize size = 0;
    GError* err = NULL;
    if (g_file_load_contents(file, cancellable, &buff, &size, NULL, &err)) [[likely]]
        return std::make_shared<GBuffWrap>(reinterpret_cast<uint8_t*>(buff), size);
    g_warning("Failed to read file: %s", err ? err->message : "unknown error");
    g_clear_error(&err);
    return nullptr;
};

enum class ImageType {
    UNKNOWN,
    BMP,
    DDS,
    GIF,
    HEIF,
    JPEG,
    JPEG_XL,
    PNG,
    TIFF,
    WEBP,
};

static ImageType comm_get_type_from_path(const char* path)
{
    const char* ext = strrchr(path, '.');
    if (ext) {
        if (!strcasecmp(ext, ".png"))
            return ImageType::PNG;
        if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
            return ImageType::JPEG;
        if (!strcasecmp(ext, ".bmp"))
            return ImageType::BMP;
        if (!strcasecmp(ext, ".gif"))
            return ImageType::GIF;
        if (!strcasecmp(ext, ".tiff") || !strcasecmp(ext, ".tif"))
            return ImageType::TIFF;
        if (!strcasecmp(ext, ".webp"))
            return ImageType::WEBP;
        if (!strcasecmp(ext, ".dds"))
            return ImageType::DDS;
        if (!strcasecmp(ext, ".heif") || !strcasecmp(ext, ".heic"))
            return ImageType::HEIF;
        if (!strcasecmp(ext, ".jxl"))
            return ImageType::JPEG_XL;
    }
    return ImageType::UNKNOWN;
}

static auto comm_get_action_button(const char* title)
{
    auto* label = Gtk::make_managed<Gtk::Label>(title);
    auto* btn = Gtk::make_managed<Gtk::Button>();
    label->set_xalign(0.0);
    btn->set_child(*label);
    btn->add_css_class("flat");
    btn->set_hexpand();
    return btn;
}

static auto comm_get_filename(const Glib::ustring& path)
{
#ifdef _WIN32
    const auto slash = path.find_last_of('\\');
#else
    const auto slash = path.find_last_of('/');
#endif
    if (slash != Glib::ustring::npos)
        return path.substr(slash + 1);
    return path;
}

#pragma endregion
#pragma region image
#pragma region image_exif

typedef enum _exiv2_error {
    EXIV2_ERROR_OPEN_FAILED, // 打开文件失败
    EXIV2_ERROR_READ_METADATA, // 读取元数据失败
    EXIV2_ERROR_WRITE_METADATA, // 写入元数据失败
    EXIV2_ERROR_INVALID_FORMAT, // 无效的图片格式
    // EXIV2_ERROR_MEMORY_ALLOC,     // 内存分配失败
    EXIV2_ERROR_UNKNOWN // 未知错误
} exiv2_error_e;

#define EXIV2_GLIB_ERROR_DOMAIN g_quark_from_static_string("exiv2-glib-error")

enum class MetaRecordGroup {
    NONE,
    EXIF,
    IPTC,
    XMP,
};

enum class MetaRecordState {
    READONLY,
    EDITABLE,
    EDITED
};

struct MetaRecord : public Glib::Object {
    MetaRecordGroup group = MetaRecordGroup::NONE;
    MetaRecordState state = MetaRecordState::READONLY;
    Glib::ustring key, name, value;

    Glib::PropertyProxy<MetaRecordState> property_state() { return ps.get_proxy(); }
    Glib::PropertyProxy<Glib::ustring> property_name() { return pn.get_proxy(); }
    Glib::PropertyProxy<Glib::ustring> property_value() { return pv.get_proxy(); }

    MetaRecord(const char* n)
        : Glib::ObjectBase(typeid(MetaRecord))
        , name(n)
        , ps(*this, "state", MetaRecordState::READONLY)
        , pn(*this, "na", key)
        , pv(*this, "value", value)
    {
    }
    MetaRecord(MetaRecordGroup g, const std::string& k, const std::string& n, const std::string& v)
        : Glib::ObjectBase(typeid(MetaRecord))
        , group(g)
        , key(k)
        , name(n)
        , value(v)
        , ps(*this, "state", state)
        , pn(*this, "na", name)
        , pv(*this, "value", value)
    {
        state = check_editable(k);
    }
    static Glib::RefPtr<MetaRecord> create(const char* title)
    {
        return Glib::make_refptr_for_instance<MetaRecord>(new MetaRecord(title));
    }
    static Glib::RefPtr<MetaRecord> create(MetaRecordGroup g, const std::string& key, const std::string& name, const std::string& value)
    {
        return Glib::make_refptr_for_instance<MetaRecord>(new MetaRecord(g, key, name, value));
    }

private:
    Glib::Property<MetaRecordState> ps;
    Glib::Property<Glib::ustring> pn;
    Glib::Property<Glib::ustring> pv;

    static MetaRecordState check_editable(const std::string& k)
    {
        static const std::set<std::string> editable_keys = {
            // EXIF
            "Exif.Image.Artist",
            "Exif.Image.Copyright",
            "Exif.Image.ImageDescription",
            "Exif.Image.XPAuthor",
            "Exif.Image.XPComment",
            "Exif.Image.XPKeywords",
            "Exif.Image.XPSubject",
            "Exif.Image.XPTitle",
            "Exif.Photo.UserComment",
            // IPTC
            "Iptc.Application2.Byline",
            "Iptc.Application2.BylineTitle",
            "Iptc.Application2.Caption",
            "Iptc.Application2.Category",
            "Iptc.Application2.City",
            "Iptc.Application2.Contact",
            "Iptc.Application2.Copyright",
            "Iptc.Application2.CountryName",
            "Iptc.Application2.Credit",
            "Iptc.Application2.DateCreated",
            "Iptc.Application2.DigitizationDate",
            "Iptc.Application2.DigitizationTime",
            "Iptc.Application2.Headline",
            "Iptc.Application2.Keywords",
            "Iptc.Application2.ObjectName",
            "Iptc.Application2.Program",
            "Iptc.Application2.ProgramVersion",
            "Iptc.Application2.Source",
            "Iptc.Application2.SpecialInstructions",
            "Iptc.Application2.SuppCategory",
            "Iptc.Application2.TimeCreated",
            "Iptc.Application2.Writer",
            // XMP
            "Xmp.dc.contributor",
            "Xmp.dc.coverage",
            "Xmp.dc.creator",
            "Xmp.dc.description",
            "Xmp.dc.format",
            "Xmp.dc.identifier",
            "Xmp.dc.language",
            "Xmp.dc.publisher",
            "Xmp.dc.relation",
            "Xmp.dc.rights",
            "Xmp.dc.source",
            "Xmp.dc.subject",
            "Xmp.dc.title",
            "Xmp.dc.type",
            "Xmp.exif.DateTimeOriginal",
            "Xmp.exif.UserComment",
            "Xmp.iptc.CopyrightNotice",
            "Xmp.iptc.CreatorContactInfo",
            "Xmp.iptc.IntellectualGenre",
            "Xmp.iptc.Location",
            "Xmp.iptc.Scene",
            "Xmp.iptc.SubjectCode",
            "Xmp.photoshop.CaptionWriter",
            "Xmp.photoshop.Category",
            "Xmp.photoshop.City",
            "Xmp.photoshop.Country",
            "Xmp.photoshop.Credit",
            "Xmp.photoshop.Headline",
            "Xmp.photoshop.Instructions",
            "Xmp.photoshop.Source",
            "Xmp.photoshop.State",
            "Xmp.photoshop.SupplementalCategories",
            "Xmp.tiff.Artist",
            "Xmp.tiff.Copyright",
            "Xmp.tiff.ImageDescription",
            "Xmp.xmp.Rating"
        };
        return editable_keys.contains(k) ? MetaRecordState::EDITABLE : MetaRecordState::READONLY;
    }
};

struct MetaRecordComparer {
    bool operator()(const Glib::RefPtr<MetaRecord>& a, const Glib::RefPtr<MetaRecord>& b) const
    {
        if (a && b)
            return a->key < b->key;
        return false;
    }
};

struct Metadata {
    std::vector<Glib::RefPtr<MetaRecord>> exif, iptc, xmp;
    bool have_value() const
    {
        return exif.size() || iptc.size() || xmp.size();
    }
    Glib::RefPtr<Glib::Bytes> save(const uint8_t* bytes, size_t size, GError** err) const
    {
        try {
            auto img = Exiv2::ImageFactory::open(bytes, size);
            img->readMetadata();

            Exiv2::ExifData newExif;
            for (const auto& dt : exif)
                newExif[dt->key] = dt->value;
            img->setExifData(newExif);

            Exiv2::IptcData newIptc;
            for (const auto& dt : iptc)
                newIptc[dt->key] = dt->value;
            img->setIptcData(newIptc);

            Exiv2::XmpData newXmp;
            for (const auto& dt : xmp)
                newXmp[dt->key] = dt->value;
            img->setXmpData(newXmp);
            img->writeMetadata();

            Exiv2::BasicIo& io = img->io();
            io.seek(0, Exiv2::BasicIo::beg);

            size = io.size();
            uint8_t* buff = reinterpret_cast<uint8_t*>(g_malloc(size));
            io.read(buff, size);
            return Glib::wrap(g_bytes_new_take(buff, size));
        } catch (const Exiv2::Error& e) {
            if (err)
                g_set_error(err, EXIV2_GLIB_ERROR_DOMAIN, static_cast<int>(e.code()), "Exiv2 error: %s", e.what());
            return nullptr;
        } catch (const std::exception& e) {
            if (err)
                g_set_error(err, EXIV2_GLIB_ERROR_DOMAIN, EXIV2_ERROR_UNKNOWN, "Standard exception: %s", e.what());
            return nullptr;
        }
    }
    static std::shared_ptr<Metadata> create(const uint8_t* bytes, size_t size, GError** err)
    {
        try {
            auto img = Exiv2::ImageFactory::open(bytes, size);
            if (!(img && img->good())) {
                if (err)
                    g_set_error(err, EXIV2_GLIB_ERROR_DOMAIN, -1, "Failed to open image from memory");
                return nullptr;
            }
            img->readMetadata();
            auto rev = std::make_shared<Metadata>();
            Exiv2::ExifData& exif = img->exifData();
            std::set<Glib::RefPtr<MetaRecord>, MetaRecordComparer> items;
            for (auto& dt : exif) {
                auto it = items.insert(MetaRecord::create(MetaRecordGroup::EXIF, dt.key(), dt.tagLabel(), dt.toString()));
                if (!it.second) {
                    (*it.first)->value += "; " + dt.toString();
                }
            }
            rev->exif.insert(rev->exif.end(), std::move_iterator(items.begin()), std::move_iterator(items.end()));

            Exiv2::IptcData& iptc = img->iptcData();
            for (auto& dt : iptc) {
                auto it = items.insert(MetaRecord::create(MetaRecordGroup::IPTC, dt.key(), dt.tagLabel(), dt.toString()));
                if (!it.second) {
                    (*it.first)->value += "; " + dt.toString();
                }
            }
            rev->iptc.insert(rev->iptc.end(), std::move_iterator(items.begin()), std::move_iterator(items.end()));

            Exiv2::XmpData& xmp = img->xmpData();
            for (auto& dt : xmp) {
                auto it = items.insert(MetaRecord::create(MetaRecordGroup::XMP, dt.key(), dt.tagLabel(), dt.toString()));
                if (!it.second) {
                    (*it.first)->value += "; " + dt.toString();
                }
            }
            rev->xmp.insert(rev->xmp.end(), std::move_iterator(items.begin()), std::move_iterator(items.end()));
            return rev;
        } catch (const Exiv2::Error& e) {
            if (err)
                g_set_error(err, EXIV2_GLIB_ERROR_DOMAIN, static_cast<int>(e.code()), "Exiv2 error: %s", e.what());
            return nullptr;
        } catch (const std::exception& e) {
            if (err)
                g_set_error(err, EXIV2_GLIB_ERROR_DOMAIN, EXIV2_ERROR_UNKNOWN, "Standard exception: %s", e.what());
            return nullptr;
        }
    }
};

struct Frame {
    Glib::ustring path;
    std::shared_ptr<Metadata> meta = nullptr;
    Glib::RefPtr<Glib::Bytes> pixels = nullptr;
    Glib::RefPtr<Gdk::Texture> texture = nullptr;
    // ImageType type = ImageType::UNKNOWN;
    uint32_t width = 0, height = 0, duration = 0;
    // 禁止拷贝（因为 Gdk::Texture 不容易拷贝）
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Glib::ustring p, void* b, uint32_t w, uint32_t h)
        : path(p)
        , width(w)
        , height(h)
    {
        pixels = Glib::wrap(g_bytes_new_take(b, width * height * 4));
        // type = comm_get_type_from_path(p.c_str());
        // 26-3-15
        // 程序在加载完备后崩溃，位置在 gdk_*
        // GPT: GTK/GDK 对象通常必须在主线程创建/操作
        // gemini: 强烈建议不要在子线程中创建 GTK/Glib 对象（会导致难以排查的跨线程和引用计数崩溃）
        // 移除 Gdk::MemoryTexture::create 后修复
    }
    Frame(Glib::ustring p, void* b, uint32_t w, uint32_t h, uint32_t d)
        : path(p)
        , width(w)
        , height(h)
        , duration(d)
    {
        pixels = Glib::wrap(g_bytes_new_take(b, width * height * 4));
    }
    void ensure_texture()
    {
        if (!texture && pixels && width && height)
            texture = Gdk::MemoryTexture::create(width, height, Gdk::MemoryTexture::Format::R8G8B8A8, pixels, width * 4);
    }
    Frame(Frame&& other) noexcept
        : path(std::move(other.path))
        , meta(std::move(other.meta))
        , pixels(std::move(other.pixels))
        , texture(std::move(other.texture))
        , width(other.width)
        , height(other.height)
        , duration(other.duration)
    {
        other.width = 0;
        other.height = 0;
        other.duration = 0;
    }
    Frame& operator=(Frame&& other) noexcept
    {
        if (this != &other) {
            path = std::move(other.path);
            meta = std::move(other.meta);
            pixels = std::move(other.pixels);
            texture = std::move(other.texture);
            width = other.width;
            height = other.height;
            duration = other.duration;
            other.width = 0;
            other.height = 0;
            other.duration = 0;
        }
        return *this;
    }
};

struct MetaRecordWidget : public Gtk::Stack {
    Gtk::Label lbTitle, lbKey, lbValue;
    Gtk::Entry ipValue;
    Gtk::Stack stack;
    Glib::RefPtr<MetaRecord> item;
    sigc::connection conn;
    MetaRecordWidget()
        : Gtk::Stack()
    {
        lbTitle.add_css_class("title");
        lbTitle.set_xalign(0.0);

        lbKey.add_css_class("key");
        lbKey.set_xalign(0.0);
        lbKey.set_size_request(230);
        lbKey.set_hexpand_set(true);
        lbKey.set_ellipsize(Pango::EllipsizeMode::END);

        lbValue.add_css_class("value");
        lbValue.set_xalign(0.0);
        lbValue.set_ellipsize(Pango::EllipsizeMode::START);
        lbValue.set_selectable(true);
        lbValue.set_hexpand(true);

        ipValue.add_css_class("value");
        ipValue.set_hexpand(true);

        stack.add(lbValue, "label");
        stack.add(ipValue, "input");

        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
        box->add_css_class("field");
        box->append(lbKey);
        box->append(stack);

        add_css_class("meta-row");
        add(lbTitle, "title");
        add(*box, "field");
        set_hexpand_set(true);
    }
    void bind(const Glib::RefPtr<MetaRecord>& it)
    {
        item = it;
        if (!it)
            return;

        if (MetaRecordGroup::NONE == it->group) {
            lbTitle.set_text(it->name);
            return;
        }
        set_visible_child("field");
        lbKey.set_text(it->name);
        lbKey.set_tooltip_text(it->key);
        if (MetaRecordState::READONLY != it->state) {
            ipValue.set_text(it->value);
            ipValue.set_sensitive(true);
            stack.set_visible_child("input");
            conn = ipValue.signal_changed().connect([this] {
                if (item && MetaRecordState::READONLY != item->state)
                    item->value = ipValue.get_text();
            });
            return;
        }
        lbValue.set_text(it->value);
        ipValue.set_sensitive(false);
        stack.set_visible_child("label");
    }
    void unbind()
    {
        if (item && MetaRecordGroup::NONE != item->group && conn.connected())
            conn.disconnect();
    }
};

#ifdef JXLL_BUILD_WITH_WIC
using Microsoft::WRL::ComPtr;

struct ComGuard {
    ComGuard()
    {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    }
    ~ComGuard()
    {
        CoUninitialize();
    }
};

static std::optional<std::vector<Frame>> wic_read_bitmap_core(const uint8_t* buff, size_t size, const char* path, GCancellable* cancellable)
{
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return std::nullopt;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)))
        return std::nullopt;
    if (FAILED(stream->InitializeFromMemory(const_cast<uint8_t*>(buff), static_cast<DWORD>(size))))
        return std::nullopt;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), NULL, WICDecodeMetadataCacheOnDemand, &decoder)))
        return std::nullopt;

    uint32_t frameCount = 0;
    if (FAILED(decoder->GetFrameCount(&frameCount)) || !frameCount)
        return std::nullopt;

    std::vector<Frame> rev;
    rev.reserve(frameCount);

    for (uint32_t i = 0; i < frameCount; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(i, &frame)))
            return std::nullopt;

        // ComPtr<IWICMetadataQueryReader> metaReader;
        // if (SUCCEEDED(frame->QueryInterface(IID_IWICMetadataQueryReader, (void**)&metaReader)))
        //     metadata = Metadata(pathStr, metaReader.Get());
        //
        // uint32_t frameDelay = 20;
        // if (type == ImageType::GIF) {
        //     ComPtr<IWICMetadataQueryReader> gifReader;
        //     if (SUCCEEDED(frame->QueryInterface(IID_IWICMetadataQueryReader, (void**)&gifReader))) {
        //         PROPVARIANT prop;
        //         PropVariantInit(&prop);
        //         if (SUCCEEDED(gifReader->GetMetadataByName(L"/grctlext/Delay", &prop))) {
        //             if (prop.vt == VT_UI2 || prop.vt == VT_I4) {
        //                 uint32_t delay = (prop.vt == VT_UI2) ? prop.uiVal : prop.lVal;
        //                 if (delay > 0) {
        //                     frameDelay = delay * 10;
        //                 }
        //             }
        //         }
        //         PropVariantClear(&prop);
        //     }
        // }

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)))
            return std::nullopt;
        if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeCustom)))
            return std::nullopt;
        uint32_t width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height)))
            return std::nullopt;

        uint32_t stride = width * 4;
        uint32_t buffSize = stride * height;
        uint8_t* pixels = static_cast<uint8_t*>(g_malloc(buffSize));
        if (FAILED(converter->CopyPixels(NULL, stride, buffSize, pixels))) {
            g_free(pixels);
            return std::nullopt;
        }
        rev.emplace_back(path, pixels, width, height);
    }
    return rev;
}

static bool wic_write_bitmap_core(const Frame& frame, GFile* file, uint32_t quality, GCancellable* cancellable)
{
    if (!(frame.pixels && frame.width && frame.height))
        return false;

    const CLSID* eid = nullptr;
    const char* path = g_file_peek_path(file);
    ImageType type = comm_get_type_from_path(path);
    if (ImageType::PNG == type) {
        eid = &CLSID_WICPngEncoder;
    } else if (ImageType::JPEG == type) {
        eid = &CLSID_WICJpegEncoder;
    } else if (ImageType::BMP == type) {
        eid = &CLSID_WICBmpEncoder;
    } else {
        g_warning("Unsupported image type for WIC write");
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IWICStream> wicStream;
    if (FAILED(factory->CreateStream(&wicStream)))
        return false;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!hMem)
        return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, stream.GetAddressOf()))) {
        GlobalFree(hMem);
        return false;
    }
    if (FAILED(wicStream->InitializeFromIStream(stream.Get())))
        return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(*eid, nullptr, &encoder)))
        return false;
    if (FAILED(encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache)))
        return false;

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    if (FAILED(encoder->CreateNewFrame(&frameEncode, nullptr)))
        return false;
    if (FAILED(frameEncode->Initialize(nullptr)))
        return false;

    if (FAILED(frameEncode->SetSize(frame.width, frame.height)))
        return false;

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frameEncode->SetPixelFormat(&pixelFormat)))
        return false;

    gsize pixelSize = 0;
    const void* pixels = frame.pixels->get_data(pixelSize);
    uint32_t stride = ((frame.width * 4) + 3) & ~3;
    if (FAILED(frameEncode->WritePixels(frame.height, stride, stride * frame.height, (uint8_t*)pixels)))
        return false;

    if (FAILED(frameEncode->Commit()))
        return false;
    if (FAILED(encoder->Commit()))
        return false;

    size_t memSize = GlobalSize(hMem);
    LPVOID pMem = GlobalLock(hMem);
    if (!pMem)
        return false;

    GError* err = NULL;
    bool succ = g_file_replace_contents(file, (const char*)pMem, memSize, NULL, false, G_FILE_CREATE_NONE, NULL, cancellable, &err);
    GlobalUnlock(hMem);
    if (succ)
        return true;

    g_warning("Failed to write file: %s", err ? err->message : "Unknown error");
    g_clear_error(&err);
    return false;
}

static std::optional<std::vector<Frame>> comm_read_bitmap_core(const uint8_t* buff, size_t size, const char* path, GCancellable* cancellable)
{
    return wic_read_bitmap_core(buff, size, path, cancellable).and_then([buff, size](auto&& fs) {
        GError* err = NULL;
        fs[0].meta = Metadata::create(buff, size, &err);
        if (err) {
            g_warning("Failed to read metadata: %s", err->message);
            g_error_free(err);
        }
        return std::make_optional(std::move(fs));
    });
}

#define LibraryGuard ComGuard guard;
#define comm_write_bitmap_core(frame, file, quality, cancellable) wic_write_bitmap_core(frame, file, quality, cancellable)
#define comm_write_bitmap_animation_core(frames, file, quality, cancellable) false
#elif defined(JXLL_BUILD_WITH_FREEIMAGE)
#include <FreeImagePlus.h>
#include <cpuid.h>
#include <functional>
#include <immintrin.h>

struct FreeImageGuard {
    FreeImageGuard()
    {
        FreeImage_Initialise(TRUE);
    }
    ~FreeImageGuard()
    {
        FreeImage_DeInitialise();
    }
};

#define LibraryGuard FreeImageGuard guard;

struct CoverArgs {
    const uint8_t* src = nullptr;
    uint8_t* dst = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
};

static struct FIFCover {
    std::function<uint8_t*(const CoverArgs&)> cover = [](const CoverArgs& args) {
        const uint8_t* src;
        uint8_t* dst;
        uint32_t idx;
        for (uint32_t y = 0; y < args.height; y++) {
            src = args.src + (args.height - 1 - y) * args.pitch;
            dst = args.dst + y * args.pitch;
            for (uint32_t x = 0; x < args.width; x++) {
                idx = x * 4;
                dst[idx + 0] = src[idx + 2];
                dst[idx + 1] = src[idx + 1];
                dst[idx + 2] = src[idx + 0];
                dst[idx + 3] = src[idx + 3];
            }
        }
        return args.dst;
    };
    FIFCover()
    {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        // SSSE3 (shuffle instruction, CPUID ecx bit 9)
        __get_cpuid(1, &eax, &ebx, &ecx, &edx);
        if (ecx & (1 << 9))
            cover = [](const CoverArgs& args) {
                const __m128i mask = _mm_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
                const uint8_t* src;
                uint8_t* dst;
                uint32_t idx;
                for (uint32_t y = 0; y < args.height; y++) {
                    src = args.src + (args.height - 1 - y) * args.pitch;
                    dst = args.dst + y * args.pitch;
                    uint32_t x = 0;
                    for (; x + 4 <= args.width; x += 4) {
                        __m128i vec = _mm_loadu_si128((const __m128i*)(src + x * 4));
                        vec = _mm_shuffle_epi8(vec, mask);
                        _mm_storeu_si128((__m128i*)(dst + x * 4), vec);
                    }
                    for (; x < args.width; x++) {
                        idx = x * 4;
                        dst[idx + 0] = src[idx + 2];
                        dst[idx + 1] = src[idx + 1];
                        dst[idx + 2] = src[idx + 0];
                        dst[idx + 3] = src[idx + 3];
                    }
                }
                return args.dst;
            };
        // AVX2 (CPUID leaf 7, subleaf 0, ebx bit 5)
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        if (ebx & (1 << 5))
            cover = [](const CoverArgs& args) {
                // clang-format off
            __m256i mask = _mm256_setr_epi8(
                2,  1,  0,  3,   6,  5,  4,  7,   10, 9,  8,  11,  14, 13, 12, 15,
                18, 17, 16, 19,  22, 21, 20, 23,  26, 25, 24, 27,  30, 29, 28, 31
            );
                // clang-format on
                const uint8_t* src;
                uint8_t* dst;
                uint32_t idx;
                for (uint32_t y = 0; y < args.height; y++) {
                    src = args.src + (args.height - 1 - y) * args.pitch;
                    dst = args.dst + y * args.pitch;
                    uint32_t x = 0;
                    for (; x + 8 <= args.width; x += 8) {
                        __m256i vec = _mm256_loadu_si256((__m256i*)(src + x * 4));
                        vec = _mm256_shuffle_epi8(vec, mask);
                        _mm256_storeu_si256((__m256i*)(dst + x * 4), vec);
                    }
                    for (; x < args.width; x++) {
                        idx = x * 4;
                        dst[idx + 0] = src[idx + 2];
                        dst[idx + 1] = src[idx + 1];
                        dst[idx + 2] = src[idx + 0];
                        dst[idx + 3] = src[idx + 3];
                    }
                }
                return args.dst;
            };
    }
    uint8_t* operator()(const CoverArgs& args) { return cover(args); }
} coverToRGBA;

// RAII 包装器：自动管理 FIMEMORY
struct FIMemoryGuard {
    FIMEMORY* mem;
    explicit FIMemoryGuard(FIMEMORY* m)
        : mem(m)
    {
    }
    ~FIMemoryGuard()
    {
        if (mem)
            FreeImage_CloseMemory(mem);
    }
    FIMemoryGuard(const FIMemoryGuard&) = delete;
    FIMemoryGuard& operator=(const FIMemoryGuard&) = delete;
    FIMemoryGuard(FIMemoryGuard&& other) noexcept
        : mem(other.mem)
    {
        other.mem = nullptr;
    }
    FIMemoryGuard& operator=(FIMemoryGuard&& other) noexcept
    {
        if (this != &other) {
            if (mem)
                FreeImage_CloseMemory(mem);
            mem = other.mem;
            other.mem = nullptr;
        }
        return *this;
    }
    FIMEMORY* get() const { return mem; }
    explicit operator bool() const { return mem != nullptr; }
};

// RAII 包装器：自动管理 FIBITMAP
struct FIBitmapGuard {
    FIBITMAP* bmp;
    explicit FIBitmapGuard(FIBITMAP* b)
        : bmp(b)
    {
    }
    ~FIBitmapGuard()
    {
        if (bmp)
            FreeImage_Unload(bmp);
    }
    FIBitmapGuard(const FIBitmapGuard&) = delete;
    FIBitmapGuard& operator=(const FIBitmapGuard&) = delete;
    FIBitmapGuard(FIBitmapGuard&& other) noexcept
        : bmp(other.bmp)
    {
        other.bmp = nullptr;
    }
    FIBitmapGuard& operator=(FIBitmapGuard&& other) noexcept
    {
        if (this != &other) {
            if (bmp)
                FreeImage_Unload(bmp);
            bmp = other.bmp;
            other.bmp = nullptr;
        }
        return *this;
    }
    FIBITMAP* get() const { return bmp; }
    explicit operator bool() const { return bmp != nullptr; }

    // 辅助方法：转换为32位
    bool convertTo32Bits()
    {
        if (!bmp)
            return false;
        FIBITMAP* dib_32 = FreeImage_ConvertTo32Bits(bmp);
        if (!dib_32)
            return false;
        FreeImage_Unload(bmp);
        bmp = dib_32;
        return true;
    }
};

static bool freeimage_read_from_memory(const uint8_t* buff, size_t size, const char* path, std::vector<Frame>& frames)
{
    // 1. 打开内存流 - RAII自动管理
    FIMemoryGuard hmem(FreeImage_OpenMemory(const_cast<BYTE*>(buff), size));
    if (!hmem) {
        return false;
    }

    // 2. 检测文件格式
    FREE_IMAGE_FORMAT fif = FreeImage_GetFileTypeFromMemory(hmem.get(), 0);
    if (FIF_UNKNOWN == fif) {
        fif = FreeImage_GetFileType(path, 0);
    }
    if (FIF_UNKNOWN == fif) {
        return false;
    }

    // 3. 加载位图 - RAII自动管理
    FIBitmapGuard dib(FreeImage_LoadFromMemory(fif, hmem.get(), 0));
    if (!dib) {
        return false;
    }

    // 4. 确保是32位
    if (32 != FreeImage_GetBPP(dib.get())) {
        if (!dib.convertTo32Bits()) {
            return false;
        }
    }

    // 5. 提取图像数据
    uint32_t width = FreeImage_GetWidth(dib.get());
    uint32_t height = FreeImage_GetHeight(dib.get());
    uint8_t* pixelData = g_new(uint8_t, width * height * 4);

    // 6. 颜色空间转换 (BGRA -> RGBA)
    CoverArgs args { FreeImage_GetBits(dib.get()), pixelData, width, height, FreeImage_GetPitch(dib.get()) };
    coverToRGBA(args);

    Frame frame(path, pixelData, width, height);
    frames.push_back(std::move(frame));
    return true;
}

static bool freeimage_write_bitmap_core(const Frame& frame, GFile* file, uint32_t quality, GCancellable* cancellable)
{
    if (!(frame.pixels && frame.width && frame.height))
        return false;
    const char* path = g_file_peek_path(file);
    ImageType type = comm_get_type_from_path(path);
    if (!(ImageType::BMP == type || ImageType::JPEG == type || ImageType::PNG == type)) {
        g_warning("Unsupported image type for FreeImage write");
        return false;
    }

    gsize pixelSize = 0;
    const void* pixels = frame.pixels->get_data(pixelSize);
    if (pixelSize < frame.width * frame.height * 4)
        return false;

    FIBITMAP* dib = FreeImage_ConvertFromRawBits((uint8_t*)pixels, frame.width, frame.height, frame.width * 4, 32, FI_RGBA_RED, FI_RGBA_GREEN, FI_RGBA_BLUE, TRUE);
    if (!dib)
        return false;
    FIBitmapGuard guard(dib);

    FREE_IMAGE_FORMAT fif = FIF_UNKNOWN;
    if (ImageType::BMP == type)
        fif = FIF_BMP;
    else if (ImageType::JPEG == type)
        fif = FIF_JPEG;
    else if (ImageType::PNG == type)
        fif = FIF_PNG;

    int saveFlags = 0;
    if (fif == FIF_JPEG && quality > 0) {
        if (quality >= 8)
            saveFlags = JPEG_QUALITYSUPERB;
        else if (quality >= 5)
            saveFlags = JPEG_QUALITYGOOD;
        else if (quality >= 3)
            saveFlags = JPEG_QUALITYNORMAL;
        else
            saveFlags = JPEG_QUALITYAVERAGE;
    }

    FIMEMORY* mem = FreeImage_OpenMemory(nullptr, 0);
    if (!mem)
        return false;
    FIMemoryGuard memGuard(mem);

    if (!FreeImage_SaveToMemory(fif, dib, mem, saveFlags))
        return false;

    BYTE* data = nullptr;
    DWORD size = 0;
    FreeImage_AcquireMemory(mem, &data, &size);
    if (!data || !size)
        return false;

    GError* err = NULL;
    bool succ = g_file_replace_contents(file, (const char*)data, size, NULL, FALSE, G_FILE_CREATE_NONE, NULL, cancellable, &err);
    if (!succ) {
        g_warning("FreeImage write error: %s", err ? err->message : "Unknown error");
        g_error_free(err);
        return false;
    }
    return true;
}

static std::optional<std::vector<Frame>> comm_read_bitmap_core(const uint8_t* buff, size_t size, const char* path, GCancellable* cancellable)
{
    std::vector<Frame> frames;
    if (!freeimage_read_from_memory(buff, size, path, frames)) {
        return std::nullopt;
    }
    GError* err = NULL;
    frames[0].meta = Metadata::create(buff, size, &err);
    if (err) {
        g_warning("Failed to read metadata: %s", err->message);
        g_error_free(err);
    }
    return frames;
}

#define comm_write_bitmap_core(frame, file, quality, cancellable) freeimage_write_bitmap_core(frame, file, quality, cancellable)
#define comm_write_bitmap_animation_core(frames, file, quality, cancellable) false
#elif defined(JXLL_BUILD_WITH_IMAGEMAGICK)
#include <Magick++.h>

struct ImageMagickGuard {
    ImageMagickGuard()
    {
        Magick::InitializeMagick(nullptr);
    }
    ~ImageMagickGuard()
    {
    }
};

#define LibraryGuard ImageMagickGuard guard;

static bool imagick_read_from_memory(const uint8_t* buff, size_t size, const char* path, std::vector<Frame>& frames)
{
    LibraryGuard;
    Magick::Blob blob(buff, size);
    try {
        Magick::Image image(blob);
        size_t width = image.columns();
        size_t height = image.rows();

        uint8_t* pixelData = g_new(uint8_t, width * height * 4);
        image.write(0, 0, width, height, "BGRA", Magick::CharPixel, pixelData);

        Frame frame(path, pixelData, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        frames.push_back(std::move(frame));
        return true;
    } catch (Magick::Exception& error) {
        g_warning("ImageMagick error: %s", error.what());
        return false;
    }
}

static bool imagick_write_bitmap_core(const Frame& frame, GFile* file, uint32_t quality, GCancellable* cancellable)
{
    if (!(frame.pixels && frame.width && frame.height))
        return false;
    const char* path = g_file_peek_path(file);
    ImageType type = comm_get_type_from_path(path);
    if (!(ImageType::BMP == type || ImageType::JPEG == type || ImageType::PNG == type)) {
        g_warning("Unsupported image type for ImageMagick write");
        return false;
    }

    gsize pixelSize = 0;
    const void* pixels = frame.pixels->get_data(pixelSize);
    if (pixelSize < frame.width * frame.height * 4)
        return false;

    Magick::Image image;
    try {
        image.read(frame.width, frame.height, "BGRA", Magick::CharPixel, pixels);
        if (ImageType::JPEG == type && quality > 0) {
            int q = std::clamp(static_cast<int>(quality * 100 / 9), 1, 100);
            image.quality(q);
        }

        const char* format = nullptr;
        if (ImageType::BMP == type)
            format = "BMP";
        else if (ImageType::JPEG == type)
            format = "JPEG";
        else if (ImageType::PNG == type)
            format = "PNG";
        image.magick(format);
    } catch (Magick::Exception& error) {
        g_warning("ImageMagick error: %s", error.what());
        return false;
    }

    Magick::Blob blob;
    try {
        image.write(&blob);
    } catch (Magick::Exception& error) {
        g_warning("ImageMagick error: %s", error.what());
        return false;
    }

    GError* err = NULL;
    bool succ = g_file_replace_contents(file, (const char*)blob.data(), blob.length(), NULL, FALSE, G_FILE_CREATE_NONE, NULL, cancellable, &err);
    if (!succ) {
        g_warning("ImageMagick write error: %s", err ? err->message : "Unknown error");
        g_error_free(err);
        return false;
    }
    return true;
}

static std::optional<std::vector<Frame>> comm_read_bitmap_core(const uint8_t* buff, size_t size, const char* path, GCancellable* cancellable)
{
    std::vector<Frame> frames;
    if (!imagick_read_from_memory(buff, size, path, frames)) {
        return std::nullopt;
    }
    GError* err = NULL;
    frames[0].meta = Metadata::create(buff, size, &err);
    if (err) {
        g_warning("Failed to read metadata: %s", err->message);
        g_error_free(err);
    }
    return frames;
}

#define comm_write_bitmap_core(frame, file, quality, cancellable) imagick_write_bitmap_core(frame, file, quality, cancellable)
#define comm_write_bitmap_animation_core(frames, file, quality, cancellable) false
#else
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBI_MALLOC(x) g_malloc(x)
#define STBI_REALLOC(p, x) g_realloc(p, x)
#define STBI_FREE(p) g_free(p)
#define STBIW_MALLOC(x) g_malloc(x)
#define STBIW_REALLOC(p, x) g_realloc(p, x)
#define STBIW_FREE(p) g_free(p)
#define STBIW_MEMMOVE(dst, src, n) g_memmove(dst, src, n)
#include "stb_image.h"
#include "stb_image_write.h"

#define LibraryGuard //
/**
 * 使用 stb_image 读取位图的核心函数，返回单帧数据
 */
static std::optional<std::vector<Frame>> stb_read_bitmap_core(const uint8_t* buff, size_t size, const char* path, GCancellable* cancellable)
{
    int width = 0, height = 0, channels = 0;
    uint8_t* pixels = stbi_load_from_memory(buff, size, &width, &height, &channels, 4);
    if (!pixels) [[unlikely]]
        return std::nullopt;

    std::vector<Frame> rev;
    rev.emplace_back(path, pixels, width, height);
    return rev;
}

// WriteBitmapCore 用于 stb 分支，使用 stb_image_write 的回调函数写入
// 参数: 单帧数据, 输出文件, 图像类型, 质量
static bool stb_write_bitmap_core(const Frame& frame, GFile* file, uint32_t quality, GCancellable* cancellable)
{
    if (!(frame.pixels && frame.width && frame.height))
        return false;
    const char* path = g_file_peek_path(file);
    ImageType type = comm_get_type_from_path(path);
    if (!(ImageType::BMP == type || ImageType::JPEG == type || ImageType::PNG == type)) {
        g_warning("Unsupported image type for stbi_write");
        return false;
    }
    GError* err = NULL;
    struct Args {
        GFileOutputStream* stream;
        GCancellable* cancellable;
        GError* err;
    } args = {
        g_file_replace(file, NULL, false, G_FILE_CREATE_NONE, cancellable, &err),
        cancellable
    };
    if (err) {
        g_warning("Failed to open file %s\n%s", path, err->message);
        g_error_free(err);
        return false;
    }

    auto func = [](void* ctx, void* data, int size) {
        Args* args = static_cast<Args*>(ctx);
        gsize bytes = 0;
        if (!args->err) {
            g_output_stream_write_all(G_OUTPUT_STREAM(args->stream), data, size, &bytes, args->cancellable, &args->err);
        }
    };

    gsize size = 0;
    bool succ = false;
    const void* pixels = frame.pixels->get_data(size);
    if (ImageType::PNG == type) {
        stbi_write_png_compression_level = quality;
        succ = stbi_write_png_to_func(func, &args, frame.width, frame.height, 4, pixels, frame.width * 4);
    } else if (ImageType::JPEG == type) {
        // quality 参数范围通常是 1-100
        int q = std::clamp(static_cast<int>(quality * 100 / 9), 10, 100);
        succ = stbi_write_jpg_to_func(func, &args, frame.width, frame.height, 4, pixels, q);
    } else
        succ = stbi_write_bmp_to_func(func, &args, frame.width, frame.height, 4, pixels);
    g_object_ref(args.stream);
    if (succ && !args.err)
        return true;
    g_warning("Failed to write file %s\n%s", path, args.err ? args.err->message : "Unknown error");
    g_clear_error(&args.err);
    return false;
}

static std::optional<std::vector<Frame>> comm_read_bitmap_core(const uint8_t* buff, size_t size, const char* path, GCancellable* cancellable)
{
    return stb_read_bitmap_core(buff, size, path, cancellable).and_then([buff, size](auto&& fs) {
        GError* err = NULL;
        fs[0].meta = Metadata::create(buff, size, &err);
        if (err) {
            g_warning("Failed to read metadata: %s", err->message);
            g_error_free(err);
        }
        return std::make_optional(std::move(fs));
    });
}
#define comm_write_bitmap_core(frame, file, quality, cancellable) stb_write_bitmap_core(frame, file, quality, cancellable)
#define comm_write_bitmap_animation_core(frames, file, quality, cancellable) false

#endif
#pragma endregion
#pragma region image_jxl

static std::optional<std::vector<Frame>> jxl_read_bitmap(const uint8_t* buff, size_t size, const char* path, GCancellable* cancellable)
{
    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    JxlResizableParallelRunnerPtr runner = JxlResizableParallelRunnerMake(nullptr);

    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE)) [[unlikely]]
        return std::nullopt;

    if (JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get())) [[unlikely]]
        return std::nullopt;

    JxlBasicInfo info;
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };

    JxlDecoderSetInput(dec.get(), buff, size);

    JxlDecoderStatus st;
    std::vector<Frame> fs;

    while (!g_cancellable_is_cancelled(cancellable)) [[likely]] {
        st = JxlDecoderProcessInput(dec.get());
        if (JXL_DEC_BASIC_INFO == st) {
            if (JxlDecoderGetBasicInfo(dec.get(), &info)) [[unlikely]]
                break;
            format.num_channels = 4;

            if (info.have_animation)
                JxlResizableParallelRunnerSetThreads(runner.get(), JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
            continue;
        }
        if (JXL_DEC_FRAME == st) {
            size_t size;
            if (JxlDecoderImageOutBufferSize(dec.get(), &format, &size)) [[unlikely]]
                break;

            JxlFrameHeader header;
            uint32_t duration = 0;
            if (!JxlDecoderGetFrameHeader(dec.get(), &header))
                duration = header.duration;

            void* pixels = g_malloc(size);
            if (JxlDecoderSetImageOutBuffer(dec.get(), &format, pixels, size)) [[unlikely]] {
                g_free(pixels);
                break;
            }

            fs.emplace_back(path, pixels, info.xsize, info.ysize, duration);
            continue;
        }
        if (JXL_DEC_SUCCESS == st)
            return fs;
        if (JXL_DEC_ERROR == st) [[unlikely]]
            break;
    }
    return std::nullopt;
}

static bool jxl_write_bitmap(const Frame& frame, GFile* file, uint32_t quality, GCancellable* cancellable)
{
    g_message("jxl_write_bitmap signal frame, quality=%u", quality);
    JxlEncoderPtr enc = JxlEncoderMake(nullptr);
    JxlThreadParallelRunnerPtr runner = JxlThreadParallelRunnerMake(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    if (JxlEncoderSetParallelRunner(enc.get(), JxlThreadParallelRunner, runner.get()))
        return false;

    JxlBasicInfo info {
        .have_container = JXL_TRUE,
        .xsize = frame.width,
        .ysize = frame.height,
        .bits_per_sample = 8,
        .orientation = JXL_ORIENT_IDENTITY,
        .num_color_channels = 3,
        .num_extra_channels = 1,
        .alpha_bits = 8
    };

    if (JxlEncoderSetBasicInfo(enc.get(), &info))
        return false;
    JxlColorEncoding color {};
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc.get(), &color))
        return false;

    JxlEncoderFrameSettings* setting = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (!quality) {
        JxlEncoderSetFrameLossless(setting, JXL_TRUE);
        JxlEncoderFrameSettingsSetOption(setting, JXL_ENC_FRAME_SETTING_MODULAR, 1);
        JxlEncoderFrameSettingsSetOption(setting, JXL_ENC_FRAME_SETTING_EFFORT, 9);
    } else {
        float distance = 0.8f + (quality - 1) * (8.0f / 8.0f); // 0.8 → 8.8
        distance = std::clamp(distance, 0.8f, 10.0f);
        if (JxlEncoderSetFrameDistance(setting, distance))
            return false;
        // effort：1=最快（低压缩率），10=最慢（最高压缩率）
        // quality 越高（压缩越狠），effort 越高
        uint32_t effort = 4 + (quality * 6 / 9); // 4～10 范围
        effort = std::clamp(effort, 4u, 10u);
        JxlEncoderFrameSettingsSetOption(setting, JXL_ENC_FRAME_SETTING_EFFORT, static_cast<int>(effort));
    }

    gsize pixelSize;
    JxlPixelFormat format { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    const void* pixels = frame.pixels->get_data(pixelSize);
    if (pixelSize < frame.width * frame.height * 4)
        return false;
    if (JxlEncoderAddImageFrame(setting, &format, pixels, pixelSize)) {
        return false;
    }
    JxlEncoderCloseInput(enc.get());

    auto output = Glib::ByteArray::create();
    uint8_t buffer[0xffff];
    while (!g_cancellable_is_cancelled(cancellable)) {
        uint8_t* pOut = buffer;
        size_t availOut = sizeof(buffer);
        JxlEncoderStatus st = JxlEncoderProcessOutput(enc.get(), &pOut, &availOut);
        gsize written = sizeof(buffer) - availOut;
        if (written)
            output->append(buffer, written);
        if (JXL_ENC_NEED_MORE_OUTPUT != st)
            break;
    }
    if (g_cancellable_is_cancelled(cancellable)) {
        g_message("jxl_write_bitmap cancelled");
        return false;
    }
    GError* err = NULL;
    GFileOutputStream* stream = g_file_replace(file, NULL, false, G_FILE_CREATE_NONE, cancellable, &err);
    if (stream && g_output_stream_write_all(G_OUTPUT_STREAM(stream), output->get_data(), output->size(), &pixelSize, cancellable, &err)) {
        g_object_unref(stream);
        return true;
    }
    g_warning("Failed to open file: %s\n%s", g_file_peek_path(file), err ? err->message : "Unknown error");
    g_clear_error(&err);
    g_clear_object(&stream);
    return false;
}
#ifdef _xxx
static bool jxl_write_bitmap_animation(const std::vector<Frame>& frames, Glib::RefPtr<Gio::File> file, uint32_t quality)
{
    g_message("jxl_write_bitmap, quality=%u", quality);
    if (frames.empty())
        return false;
    JxlEncoderPtr enc = JxlEncoderMake(nullptr);
    JxlThreadParallelRunnerPtr runner = JxlThreadParallelRunnerMake(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    if (JxlEncoderSetParallelRunner(enc.get(), JxlThreadParallelRunner, runner.get()))
        return false;
    JxlPixelFormat format { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.have_container = JXL_TRUE;
    info.xsize = frames[0].width;
    info.ysize = frames[0].height;
    info.bits_per_sample = 8;
    info.num_color_channels = 3;
    info.num_extra_channels = 1;
    info.alpha_bits = 8;
    if (frames.size() > 1) {
        info.have_animation = JXL_TRUE;
        info.animation.tps_numerator = 1000;
        info.animation.tps_denominator = 1;
        info.animation.num_loops = 0;
    }
    if (JxlEncoderSetBasicInfo(enc.get(), &info))
        return false;
    JxlColorEncoding color_encoding = {};
    JxlColorEncodingSetToSRGB(&color_encoding, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc.get(), &color_encoding))
        return false;

    JxlEncoderFrameSettings* frame_settings = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    // quality 越大，文件越小，质量越低。effort 范围 1-10，对应不同的压缩速度
    // quality 1-3 -> effort 1-3 (低压缩率，高质量)
    // quality 4-6 -> effort 5-7 (中等)
    // quality 7-9 -> effort 9-10 (高压缩率，低质量)
    uint32_t effort = static_cast<uint32_t>((quality - 1) * 9 / 8) + 1; // 1-9 -> 1-10
    effort = std::clamp(effort, 1u, 10u);
    JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT, static_cast<int>(effort));

    // 使用 distance 控制质量：distance 越小质量越高 (0-15)
    // quality 1 -> distance 0.1 (最高质量)
    // quality 9 -> distance 10.0 (最低质量)
    float distance = static_cast<float>((quality - 1) * 10.0 / 8.0 + 0.1);
    distance = std::clamp(distance, 0.1f, 15.0f);
    if (JXL_ENC_SUCCESS != JxlEncoderSetFrameDistance(frame_settings, distance))
        return false;

    for (const auto& frame : frames) {
        if (frames.size() > 1) {
            JxlFrameHeader header;
            JxlEncoderInitFrameHeader(&header);
            header.duration = frame.duration;
            if (JXL_ENC_SUCCESS != JxlEncoderSetFrameHeader(frame_settings, &header))
                return false;
        }
        const size_t bytes = static_cast<size_t>(frame.width) * frame.height * 4;
        gsize size = 0;
        const void* data = frame.byte->get_data(size);
        if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(frame_settings, &format, data, bytes))
            return false;
    }
    JxlEncoderCloseInput(enc.get());

    std::vector<uint8_t> output;
    output.reserve(1024 * 1024);
    uint8_t buffer[4096];
    uint8_t* next_out = buffer;
    size_t avail_out = sizeof(buffer);

    JxlEncoderStatus process_result = JXL_ENC_NEED_MORE_OUTPUT;
    while (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
        process_result = JxlEncoderProcessOutput(enc.get(), &next_out, &avail_out);
        if (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
            size_t written = buffer + sizeof(buffer) - next_out;
            output.insert(output.end(), buffer, next_out);
            next_out = buffer;
            avail_out = sizeof(buffer);
        }
    }
    size_t written = buffer + sizeof(buffer) - next_out;
    output.insert(output.end(), buffer, buffer + written);

    auto stream = file->replace();
    if (!stream)
        return false;
    stream->write(output.data(), output.size());
    return true;
}

#endif
#pragma endregion
static std::optional<std::vector<Frame>> comm_read_bitmap(GFile* file, GCancellable* cancellable)
{
    const char* path = g_file_peek_path(file);
    const char* ext = strrchr(path, '.');

    auto buff = g_file_load_contents_wrap(file, cancellable);
    if (ext && !strcasecmp(ext, ".jxl"))
        return jxl_read_bitmap(buff->buff, buff->size, path, cancellable);
    // .and_then([buff = std::move(buff)](auto&& fs) {
    //         GError* err = NULL;
    //         auto meta = Metadata::create(buff->buff, buff->size, &err);
    //         // auto meta = Metadata::create(file, &err);
    //         if (err) {
    //             g_warning("Failed to read metadata: %s", err->message);
    //             g_error_free(err);
    //         } else if (meta) {
    //             for (auto& f : fs)
    //                 f.meta = meta;
    //         }
    //         return std::make_optional(std::move(fs));
    //     });
    return comm_read_bitmap_core(buff->buff, buff->size, path, cancellable);
}

static bool comm_write_bitmap(const Frame& frame, GFile* file, uint32_t quality, GCancellable* cancellable)
{
    const char* path = g_file_peek_path(file);
    const char* ext = strrchr(path, '.');
    if (ext && !strcasecmp(ext, ".jxl"))
        return jxl_write_bitmap(frame, file, quality, cancellable);
    return comm_write_bitmap_core(frame, file, quality, cancellable);
}
#ifdef _xxx
static bool comm_write_bitmap_animation(const std::vector<Frame>& frames, Glib::RefPtr<Gio::File> file, uint32_t quality, Glib::RefPtr<Gio::Cancellable> cancellable)
{
    if (frames.empty())
        return false;
    if (file->get_path().ends_with("jxl"))
        return jxl_write_bitmap_animation(frames, file, quality);
    return comm_write_bitmap_animation_core(frames, file, quality, cancellable);
}

#endif

struct App : public Gtk::Window {
    enum class State {
        NONE, //  no file
        READING,
        READY, //  file opened
        PLAYING,
        RUNNING, //  running
        DONE, //  done
    };
    State state = State::NONE;
    Glib::RefPtr<Gio::Cancellable> cancellable;
    Glib::RefPtr<Gtk::CssProvider> provider;
    std::vector<Frame> frames;

    struct _widgets {
        Gtk::Picture preview;
        struct _widgets_animation_config {
            Gtk::Revealer cter;
            Gtk::Button toggle { "Toggle Animation" };
            Gtk::Button remove { "Remove" };
            Gtk::SpinButton duration;
            Gtk::Scale frames;
        } anim;
        struct _widgets_control {
            Gtk::Spinner fileSpinner, runSpinner;
            Gtk::Stack fileStack, runStack;
            Gtk::Button file, run; //  open / clean

            Gtk::MenuButton menu;

            Gtk::Scale quality;
        } ctrl;
        struct _widgets_exif {
            Gtk::Revealer mask, sidebar;
            Gtk::ListView list;
            Gtk::Button close, reset, save;
            Glib::RefPtr<Gio::ListStore<MetaRecord>> store;
        } exif;
        Gtk::Label status { "No files opened" };
        Gtk::Button info;
        Gtk::Overlay overlay;

        Gtk::Overlay rootOverlay;
        Gtk::Revealer infoMaskRevealer;
        Gtk::Button infoMask;
        Gtk::Revealer infoSidebarRevealer;
        Gtk::ListView infoList;
        Gtk::Button infoClose;
        Gtk::Button infoSave;

        Glib::RefPtr<Gtk::NoSelection> infoSelection;
        Glib::RefPtr<Gtk::SignalListItemFactory> infoFactory;
    } wids;
    void size_allocate_vfunc(int width, int height, int baseline) override
    {
        Gtk::Window::size_allocate_vfunc(width, height, baseline);
        if (state == State::READY && !frames.empty())
            update_status_for_frame(get_current_frame_index());
    }
#pragma region action_state
    uint32_t get_current_frame_index() const
    {
        if (frames.empty())
            return 0;
        auto idx = static_cast<uint32_t>(wids.anim.frames.get_value());
        if (idx >= frames.size())
            idx = static_cast<uint32_t>(frames.size() - 1);
        return idx;
    }
    void update_status_for_frame(uint32_t idx)
    {
        if (idx >= frames.size()) [[unlikely]]
            return;
        const float sw = static_cast<float>(wids.preview.get_width()) / static_cast<float>(frames[idx].width);
        const float sh = static_cast<float>(wids.preview.get_height()) / static_cast<float>(frames[idx].height);
        const float scale = std::min(sw, sh);
        const uint32_t pw = static_cast<uint32_t>(std::round(0.5 + scale * static_cast<float>(frames[idx].width)));
        const uint32_t ph = static_cast<uint32_t>(std::round(0.5 + scale * static_cast<float>(frames[idx].height)));
        const auto fn = comm_get_filename(frames[idx].path);
        wids.status.set_text(Glib::ustring::sprintf("%u/%u | %u x %u (%u x %u) | %s", idx + 1, static_cast<uint32_t>(frames.size()), frames[idx].width, frames[idx].height, pw, ph, fn.c_str()));
    }
    void update_sidebar_for_frame(uint32_t idx)
    {
        if (idx >= frames.size()) [[unlikely]]
            return;
        wids.exif.store->remove_all();
        if (frames[idx].meta && frames[idx].meta->have_value()) {
            wids.exif.store->append(MetaRecord::create("EXIF"));
            for (const auto& v : frames[idx].meta->exif)
                wids.exif.store->append(v);
            wids.exif.store->append(MetaRecord::create("IPTC"));
            for (const auto& v : frames[idx].meta->iptc)
                wids.exif.store->append(v);
            wids.exif.store->append(MetaRecord::create("XMP"));
            for (const auto& v : frames[idx].meta->xmp)
                wids.exif.store->append(v);
            wids.info.set_sensitive(true);
            wids.info.set_tooltip_text("Show EXIF data");
            return;
        }
        wids.info.set_sensitive(false);
        wids.info.set_tooltip_text("This picture have not EXIF data");
    }
    void update_frame(uint32_t idx)
    {
        if (idx >= frames.size())
            return;
        wids.preview.set_paintable(frames[idx].texture);
        wids.anim.frames.set_value(idx);
        update_status_for_frame(idx);
        update_sidebar_for_frame(idx);
    }
    void set_sidebar_visible(bool visible)
    {
        wids.exif.sidebar.set_reveal_child(visible);
        wids.exif.mask.set_reveal_child(visible);
        wids.exif.mask.set_sensitive(visible);
        wids.exif.mask.set_can_focus(visible);
        wids.exif.mask.set_can_target(visible);
        if (visible) {
            wids.overlay.add_css_class("overlay");
            return;
        }
        wids.overlay.remove_css_class("overlay");
    }
    void reset_state()
    {
        // cancellable->cancel();
        frames.clear();
        wids.preview.set_paintable(nullptr);
        wids.anim.cter.set_reveal_child(false);

        wids.ctrl.file.set_sensitive();
        wids.ctrl.file.set_tooltip_text("Open");
        wids.ctrl.file.remove_css_class("busy");
        wids.ctrl.fileSpinner.stop();
        wids.ctrl.fileStack.set_visible_child("open");

        wids.ctrl.run.set_sensitive(false);
        wids.ctrl.run.set_tooltip_text("Please open file first");
        wids.ctrl.run.remove_css_class("busy");
        wids.ctrl.runSpinner.stop();
        wids.ctrl.runStack.set_visible_child("save");

        wids.ctrl.menu.set_sensitive(false);
        wids.status.set_text("No files opened");
        wids.info.set_sensitive(false);
        wids.info.set_tooltip_text("Please open file first");
        wids.info.remove_css_class("metadata-dirty");

        wids.exif.store->remove_all();
        set_sidebar_visible(false);
        state = State::NONE;
    }
#pragma endregion // action_state
#pragma region action_data
    auto get_save_dialog() const
    {
        auto filter0 = Gtk::FileFilter::create();
        filter0->set_name("JPEG XL");
        filter0->add_pattern("*.jxl");

        auto filter1 = Gtk::FileFilter::create();
        filter1->set_name("Bmp");
        filter1->add_pattern("*bmp");

        auto filter2 = Gtk::FileFilter::create();
        filter2->set_name("JPEG");
        filter2->add_pattern("*jpg");

        auto filter3 = Gtk::FileFilter::create();
        filter3->set_name("PNG");
        filter3->add_pattern("*png");

        auto filter4 = Gtk::FileFilter::create();
        filter4->set_name("Other format");
        filter4->add_pattern("*");

        auto filter = Gio::ListStore<Gtk::FileFilter>::create();
        filter->append(filter0);
        filter->append(filter1);
        filter->append(filter2);
        filter->append(filter3);
        filter->append(filter4);
        auto dialog = Gtk::FileDialog::create();
        dialog->set_title("Save images to");
        dialog->set_filters(filter);
        dialog->set_initial_name("new_image.jxl");
        return dialog;
    }
    void open_files()
    {
        auto filter0 = Gtk::FileFilter::create();
        filter0->set_name("Supported Images");
        filter0->add_pattern("*.bmp");
        filter0->add_pattern("*.gif");
        filter0->add_pattern("*.jpg");
        filter0->add_pattern("*.txt");
        filter0->add_pattern("*.jxl");
        filter0->add_pattern("*.png");

        auto filter1 = Gtk::FileFilter::create();
        filter1->set_name("All Files");
        filter1->add_pattern("*");

        auto filter = Gio::ListStore<Gtk::FileFilter>::create();
        filter->append(filter0);
        filter->append(filter1);

        auto dialog = Gtk::FileDialog::create();
        dialog->set_title("Select images to convert");
        dialog->set_filters(filter);
        cancellable->cancel();
        cancellable->reset();

        dialog->open_multiple(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& res) {
            GError* err = NULL;
            GListModel* files = gtk_file_dialog_open_multiple_finish(dialog->gobj(), res->gobj(), &err);
            if (err || !files) {
                g_warning("Failed to open file: %s", err ? err->message : "Unknown error");
                g_clear_error(&err);
                g_clear_object(&files);
                return;
            }

            state = State::READING;
            wids.ctrl.file.add_css_class("busy");
            wids.ctrl.file.set_tooltip_text("Abort");
            wids.ctrl.fileSpinner.start();
            wids.ctrl.fileStack.set_visible_child("reading");

            GFile* file;
            uint32_t cnt = g_list_model_get_n_items(files);
            std::vector<std::future<std::optional<std::vector<Frame>>>> futures;
            wids.status.set_text(Glib::ustring::sprintf("Reading %u images", cnt));
            futures.reserve(cnt);
            for (uint32_t i = 0; i < cnt; ++i) {
                file = (GFile*)g_list_model_get_item(files, i);
                futures.push_back(std::async(std::launch::async, [this, file]() {
                    auto rev = comm_read_bitmap(file, cancellable->gobj());
                    g_object_unref(file);
                    return rev;
                }));
            }
            std::thread([this, futures = std::move(futures)]() mutable {
                auto loaded = std::make_shared<std::vector<Frame>>();
                loaded->reserve(futures.size());
                for (auto& fut: futures) {
                    if (cancellable->is_cancelled()) break;
                    auto res = fut.get();
                    if (res.has_value()) {
                        auto fs = std::move(*res);
                        if (fs.size())
                            loaded->insert(loaded->end(), std::move_iterator(fs.begin()), std::move_iterator(fs.end()));
                    }
                }
                if (loaded->empty()) {
                    Glib::MainContext::get_default()->invoke([this]() {
                        reset_state();
                        sys_beep_error();
                        return false;
                    });
                    return;
                }

                Glib::MainContext::get_default()->invoke([this, loaded]() {
                    frames.clear();
                    frames.reserve(loaded->size());
                    for (auto& f : *loaded) {
                        f.ensure_texture();
                        frames.push_back(std::move(f));
                    }
                    if (frames.size() > 1) {
                        wids.anim.toggle.set_sensitive();
                        wids.anim.toggle.set_icon_name("media-playback-start");
                        wids.anim.toggle.set_tooltip_text("Play animation");

                        wids.anim.remove.set_sensitive();

                        wids.anim.duration.set_sensitive();
                        wids.anim.duration.set_value(frames[0].duration);

                        wids.anim.frames.set_sensitive();
                        wids.anim.frames.set_range(0, frames.size() - 1);

                        wids.anim.cter.set_reveal_child(true);
                    }
                    wids.ctrl.fileSpinner.stop();
                    wids.ctrl.fileStack.set_visible_child("clear");
                    wids.ctrl.file.remove_css_class("busy");
                    wids.ctrl.file.set_tooltip_text("Clean all files");

                    on_quality_change();
                    wids.ctrl.run.set_sensitive();
                    wids.ctrl.menu.set_sensitive();

                    state = State::READY; //  如果有状态，也应该在此设置
                    update_frame(0);
                    sys_beep_success();
                    return false;
                });
            }).detach();
            g_object_ref(files); }, cancellable);
    }
    void save_file()
    {
        auto dialog = get_save_dialog();
        cancellable->cancel();
        cancellable->reset();
        dialog->save(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& res) {
            GError* err = NULL;
            GFile* file = gtk_file_dialog_save_finish(dialog->gobj(), res->gobj(), &err);
            if (err || !file) {
                g_warning("Failed to save file: %s", err ? err->message : "Unknown error");
                g_clear_error(&err);
                g_clear_object(&file);
                return;
            }

            uint32_t fidx = 0;
            state = State::RUNNING;
            wids.ctrl.file.set_sensitive(false);

            wids.ctrl.runStack.set_visible_child("writing");
            wids.ctrl.runSpinner.start();
            wids.ctrl.run.add_css_class("busy");
            wids.ctrl.run.set_tooltip_text("Abort");

            wids.ctrl.menu.set_sensitive(false);
            wids.info.set_sensitive(false);
            if (frames.size() > 1) {
                fidx = static_cast<uint32_t>(wids.anim.frames.get_value());
                wids.anim.toggle.set_sensitive(false);
                wids.anim.remove.set_sensitive(false);
                wids.anim.duration.set_sensitive(false);
                wids.anim.frames.set_sensitive(false);
            }
            // 获取质量设置
            uint32_t quality = std::clamp(static_cast<uint32_t>(wids.ctrl.quality.get_value()), 0u, 9u);
            std::thread([this, fidx, file, quality]() mutable {
                Glib::MainContext::get_default()->invoke([this, file, fidx, succ = comm_write_bitmap(frames[fidx], file, quality, cancellable->gobj())] {
                    // 恢复 UI 状态
                    wids.ctrl.runSpinner.stop();
                    wids.ctrl.runStack.set_visible_child("save");
                    wids.ctrl.run.remove_css_class("busy");
                    on_quality_change();
                    wids.ctrl.file.set_sensitive(true);
                    wids.ctrl.menu.set_sensitive(true);
                    wids.info.set_sensitive(frames[fidx].meta && frames[fidx].meta->have_value());
                    if (frames.size() > 1) {
                        wids.anim.toggle.set_sensitive();
                        wids.anim.remove.set_sensitive();
                        wids.anim.duration.set_sensitive();
                        wids.anim.frames.set_sensitive();
                    }
                    state = State::READY;
                    if (succ) {
                        wids.status.set_text(Glib::ustring::sprintf("Saved to %s", g_file_peek_path(file)));
                        sys_beep_success();
                    } else {
                        wids.status.set_text("Save failed");
                        sys_beep_error();
                    }
                    Glib::signal_timeout().connect_seconds_once([this] {
                        update_status_for_frame(get_current_frame_index());
                    },
                        2);
                    g_object_unref(file);
                    return false;
                });
            }).detach();
        });
    }
    void save_file_all()
    {
        auto dialog = Gtk::FileDialog::create();
        dialog->set_title("Save images to");
        cancellable->cancel();
        cancellable->reset();
        dialog->select_folder(*this, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& res) {
            GError* err = NULL;
            GFile* path = gtk_file_dialog_select_folder_finish(dialog->gobj(), res->gobj(), &err);
            if (err || !path) {
                g_warning("Failed to save file: %s", err ? err->message : "Unknown error");
                g_error_free(err);
                g_object_unref(path);
                return;
            }

            state = State::RUNNING;
            wids.anim.toggle.set_sensitive(false);
            wids.anim.remove.set_sensitive(false);
            wids.anim.duration.set_sensitive(false);
            wids.anim.frames.set_sensitive(false);

            wids.ctrl.file.set_sensitive(false);

            wids.ctrl.runStack.set_visible_child("writing");
            wids.ctrl.runSpinner.start();
            wids.ctrl.run.add_css_class("busy");
            wids.ctrl.run.set_tooltip_text("Abort");

            wids.ctrl.menu.set_sensitive(false);
            wids.status.set_text(Glib::ustring::sprintf("Saving %u images...", static_cast<uint32_t>(frames.size())));
            wids.info.set_sensitive(false);

            // 获取质量设置
            uint32_t quality = std::clamp(static_cast<uint32_t>(wids.ctrl.quality.get_value()), 0u, 9u);
            std::thread([this, path, quality]() mutable {
                std::map<Glib::ustring, uint32_t> kvs;
                for (const auto& f : frames) {
#ifdef _WIN32
                    auto pos = f.path.rfind('\\');
#else
                    auto pos = f.path.rfind('/');
#endif
                    kvs[f.path.substr(pos, f.path.rfind('.') - pos)]++;
                }

                std::vector<GFileWrap> files;
                files.reserve(frames.size());
                for (const auto& it : kvs) {
                    if (it.second) {
                        for (uint32_t i = 0; i < it.second; i++)
                            files.emplace_back(g_strdup_printf("%s%s_%u.jxl", g_file_peek_path(path), it.first.c_str(), i));
                        continue;
                    }
                    files.emplace_back(g_strdup_printf("%s%s.jxl", g_file_peek_path(path), it.first.c_str()));
                }

                std::vector<std::future<bool>> futures;
                futures.reserve(frames.size());
                for (uint32_t i = 0; i < frames.size(); i++) {
                    futures.push_back(std::async(std::launch::async, [this, i, f = files[i].file, quality] {
                        return comm_write_bitmap(frames[i], f, quality, cancellable->gobj());
                    }));
                }

                uint32_t succ = 0;
                for (auto& fut : futures) {
                    if (cancellable->is_cancelled())
                        break;
                    if (fut.get())
                        succ++;
                }
                g_object_unref(path);
                Glib::MainContext::get_default()->invoke([this, succ] {
                    wids.ctrl.runSpinner.stop();
                    wids.ctrl.runStack.set_visible_child("save");
                    wids.ctrl.run.remove_css_class("busy");
                    on_quality_change();
                    wids.anim.toggle.set_sensitive(true);
                    wids.anim.remove.set_sensitive(true);
                    wids.anim.duration.set_sensitive(true);
                    wids.anim.frames.set_sensitive(true);
                    wids.ctrl.file.set_sensitive(true);
                    wids.ctrl.menu.set_sensitive(true);
                    uint32_t idx = get_current_frame_index();
                    wids.info.set_sensitive(frames[idx].meta && frames[idx].meta->have_value());
                    wids.status.set_text(Glib::ustring::sprintf("Saved %u/%u images", succ, static_cast<uint32_t>(frames.size())));
                    Glib::signal_timeout().connect_seconds_once([this] {
                        update_status_for_frame(get_current_frame_index());
                    },
                        2);
                    sys_beep_success();
                    return false;
                });
            }).detach();
        });
    }
#pragma endregion // action_data
#pragma region action_callback
    void on_frame_change()
    {
        auto idx = static_cast<uint32_t>(wids.anim.frames.get_value());
        if (idx >= frames.size())
            return;
        update_frame(idx);
    }
    void on_info_save()
    {
        auto idx = get_current_frame_index();
        if (idx >= frames.size())
            return;
        auto& frame = frames[idx];
        if (!frame.meta || !frame.meta->have_value())
            return;

        auto buff = g_file_load_contents_wrap(g_file_new_for_path(frame.path.c_str()), cancellable->gobj());
        if (!buff) {
            sys_beep_error();
            return;
        }

        GError* err = NULL;
        auto newBuff = frame.meta->save(buff->buff, buff->size, &err);
        if (err) {
            g_warning("Failed to save metadata: %s", err->message);
            g_error_free(err);
            sys_beep_error();
            return;
        }
        if (!newBuff) {
            sys_beep_error();
            return;
        }

        auto file = g_file_new_for_path(frame.path.c_str());
        gsize size = 0;
        const void* data = newBuff->get_data(size);
        GError* writeErr = NULL;
        bool succ = g_file_replace_contents(file, (const char*)data, size, NULL, FALSE, G_FILE_CREATE_NONE, NULL, cancellable->gobj(), &writeErr);
        g_object_unref(file);
        if (!succ) {
            g_warning("Failed to save file: %s", writeErr ? writeErr->message : "Unknown error");
            if (writeErr)
                g_error_free(writeErr);
            sys_beep_error();
            return;
        }

        wids.info.remove_css_class("metadata-dirty");
        wids.status.set_text(Glib::ustring::sprintf("Metadata saved to %s", frame.path.c_str()));
        sys_beep_success();
        Glib::signal_timeout().connect_seconds_once([this] {
            update_status_for_frame(get_current_frame_index());
        },
            2);
    }
    void on_file_click()
    {
        if (State::NONE != state) {
            cancellable->cancel();
            reset_state();
            return;
        }
        open_files();
    }
    void on_quality_change()
    {
        uint32_t quality = static_cast<uint32_t>(wids.ctrl.quality.get_value());
        if (quality > 6) {
            wids.ctrl.run.set_tooltip_text(Glib::ustring::sprintf("Save current frame with level: %u (low quality, small file size)", quality));
            wids.ctrl.quality.set_tooltip_text(Glib::ustring::sprintf("compress level: %u (low quality, small file size)", quality));
        } else if (quality > 3) {
            wids.ctrl.run.set_tooltip_text(Glib::ustring::sprintf("Save current frame with level: %u (middle quality and file size)", quality));
            wids.ctrl.quality.set_tooltip_text(Glib::ustring::sprintf("compress level: %u (middle quality and file size)", quality));
        } else if (!quality) {
            wids.ctrl.run.set_tooltip_text("Save current frame lossless or highest quality");
            wids.ctrl.quality.set_tooltip_text("lossless or highest quality");
        } else {
            wids.ctrl.run.set_tooltip_text(Glib::ustring::sprintf("Save current frame with level: %u (high quality, large file size)", quality));
            wids.ctrl.quality.set_tooltip_text(Glib::ustring::sprintf("compress level: %u (high quality, large file size)", quality));
        }
    }
    void on_act_save_current()
    {
        if (!(static_cast<int>(state) && frames.size()))
            return;
        if (State::RUNNING != state) {
            save_file();
            return;
        }
        cancellable->cancel();
    }
    void on_act_save_all()
    {
        if (!(static_cast<int>(state) && frames.size()))
            return;
        if (State::RUNNING != state) {
            if (frames.size() > 1)
                save_file_all();
            else
                save_file();
            return;
        }
        cancellable->cancel();
    }
    void on_act_save_animation() { printf("%s\n", __func__); }
#pragma endregion // action_callback
#pragma region init
    void init_animation(Gtk::Box* parent)
    {
        wids.anim.toggle.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        // wids.anim.toggle.signal_clicked().connect(sigc::mem_fun(*this, &App::on_toggle_click));

        wids.anim.remove.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        wids.anim.remove.set_icon_name("edit-clear");
        wids.anim.remove.set_tooltip_text("Remove current frame");

        wids.anim.duration.set_digits(0);
        wids.anim.duration.set_numeric();
        wids.anim.duration.set_range(10, 100);
        wids.anim.duration.set_value(20);
        wids.anim.duration.set_increments(10, 10);
        wids.anim.duration.set_size_request(120);

        wids.anim.frames.set_digits(0);
        wids.anim.frames.set_round_digits(0);
        wids.anim.frames.set_increments(1, 5);
        wids.anim.frames.set_hexpand();
        wids.anim.frames.signal_value_changed().connect(sigc::mem_fun(*this, &App::on_frame_change));

        auto* boxAnim = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
        boxAnim->set_size_request(-1, DEF_ROW_HEIGHT);
        boxAnim->append(wids.anim.toggle);
        boxAnim->append(wids.anim.remove);
        boxAnim->append(wids.anim.duration);
        boxAnim->append(wids.anim.frames);
        wids.anim.cter.set_child(*boxAnim);
        parent->append(wids.anim.cter);
    }
    void init_control(Gtk::Box* parent)
    {
        auto _get_action = [](const char* name, const char* title, Glib::RefPtr<Gio::SimpleActionGroup> group, Gtk::ListBox* list, std::function<void(const Glib::VariantBase&)>&& cba, std::function<void()>&& cbb) {
            auto act = Gio::SimpleAction::create(name);
            auto* btn = comm_get_action_button(title);
            auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
            act->signal_activate().connect(cba);
            btn->signal_clicked().connect(cbb);
            row->set_child(*btn);

            group->add_action(act);
            list->append(*row);
        };
        auto group = Gio::SimpleActionGroup::create();
        auto* list = Gtk::make_managed<Gtk::ListBox>();
        _get_action("save-current", "Save Current Frame", group, list, [this](const Glib::VariantBase&) { on_act_save_current(); }, std::bind(&App::on_act_save_current, this));
        _get_action("save-all", "Save All Frames", group, list, [this](const Glib::VariantBase&) { on_act_save_all(); }, std::bind(&App::on_act_save_all, this));
        _get_action("save-anim", "Save as Animation", group, list, [this](const Glib::VariantBase&) { on_act_save_animation(); }, std::bind(&App::on_act_save_animation, this));
        list->set_selection_mode(Gtk::SelectionMode::NONE);

        insert_action_group("win", group);
        wids.ctrl.fileStack.add(*Gtk::make_managed<Gtk::Image>(Gio::Icon::create("document-open")), "open");
        wids.ctrl.fileStack.add(*Gtk::make_managed<Gtk::Image>(Gio::Icon::create("edit-clear-all")), "clear");
        wids.ctrl.fileStack.add(wids.ctrl.fileSpinner, "reading");
        wids.ctrl.file.add_css_class("io-action");
        wids.ctrl.file.set_name("wids.ctrl.file");
        wids.ctrl.file.set_child(wids.ctrl.fileStack);
        wids.ctrl.file.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        wids.ctrl.file.set_tooltip_text("Open");
        wids.ctrl.file.signal_clicked().connect(sigc::mem_fun(*this, &App::on_file_click));

        wids.ctrl.runStack.add(*Gtk::make_managed<Gtk::Image>(Gio::Icon::create("document-save")), "save");
        wids.ctrl.runStack.add(wids.ctrl.runSpinner, "writing");
        wids.ctrl.run.add_css_class("io-action");
        wids.ctrl.run.set_name("wids.ctrl.run");
        wids.ctrl.run.set_child(wids.ctrl.runStack);
        wids.ctrl.run.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        wids.ctrl.run.set_sensitive(false);
        wids.ctrl.run.set_tooltip_text("Please open file first");
        wids.ctrl.run.signal_clicked().connect(sigc::mem_fun(*this, &App::on_act_save_current));

        wids.ctrl.quality.set_digits(0);
        wids.ctrl.quality.set_round_digits(0);
        wids.ctrl.quality.set_range(0, 9);
        wids.ctrl.quality.signal_value_changed().connect(sigc::mem_fun(*this, &App::on_quality_change));

        auto* boxMenu = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 5);
        boxMenu->set_size_request(160);
        boxMenu->append(wids.ctrl.quality);
        boxMenu->append(*list);

        auto* popover = Gtk::make_managed<Gtk::Popover>();
        popover->set_position(Gtk::PositionType::TOP);
        popover->set_child(*boxMenu);
        wids.ctrl.menu.set_popover(*popover);
        wids.ctrl.menu.set_direction(Gtk::ArrowType::UP);
        wids.ctrl.menu.set_icon_name("view-more"); // open-menu-symbolic
        wids.ctrl.menu.set_sensitive(false);

        auto* boxSave = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        boxSave->add_css_class("linked");
        boxSave->append(wids.ctrl.run);
        boxSave->append(wids.ctrl.menu);

        wids.status.set_xalign(0.0);
        wids.status.set_hexpand();

        wids.info.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        wids.info.set_icon_name("dialog-information");
        wids.info.set_tooltip_text("No metadata");
        wids.info.set_sensitive(false);
        wids.info.signal_clicked().connect([this] {
            set_sidebar_visible(true);
        });

        auto* boxCtrl = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
        boxCtrl->set_size_request(-1, DEF_ROW_HEIGHT);
        boxCtrl->append(wids.ctrl.file);
        boxCtrl->append(*boxSave);
        boxCtrl->append(wids.status);
        boxCtrl->append(wids.info);
        parent->append(*boxCtrl);
    }
    void init_sidebar(Gtk::Box* parent)
    {
        auto factory = Gtk::SignalListItemFactory::create();
        factory->signal_setup().connect([](const Glib::RefPtr<Gtk::ListItem>& item) {
            item->set_child(*Gtk::make_managed<MetaRecordWidget>());
        });
        factory->signal_bind().connect([](const Glib::RefPtr<Gtk::ListItem>& item) {
            auto it = std::dynamic_pointer_cast<MetaRecord>(item->get_item());
            auto* wid = dynamic_cast<MetaRecordWidget*>(item->get_child());
            if (wid && it)
                wid->bind(it);
        });
        factory->signal_unbind().connect([](const Glib::RefPtr<Gtk::ListItem>& item) {
            auto* wid = dynamic_cast<MetaRecordWidget*>(item->get_child());
            if (wid)
                wid->unbind();
        });

        wids.exif.store = Gio::ListStore<MetaRecord>::create();
        auto model = Gtk::NoSelection::create(wids.exif.store);
        wids.exif.list.set_model(model);
        wids.exif.list.set_factory(factory);

        auto* scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
        scrolled->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scrolled->set_vexpand();
        scrolled->set_child(wids.exif.list);

        wids.exif.close.set_icon_name("window-close-symbolic");
        wids.exif.reset.set_icon_name("edit-undo-symbolic");
        wids.exif.save.set_icon_name("document-save-symbolic");
        wids.exif.close.set_tooltip_text("Close");
        wids.exif.reset.set_tooltip_text("Reset");
        wids.exif.save.set_tooltip_text("Save");
        wids.exif.close.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        wids.exif.reset.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        wids.exif.save.set_size_request(DEF_ROW_HEIGHT, DEF_ROW_HEIGHT);
        wids.exif.close.signal_clicked().connect([this]() { set_sidebar_visible(false); });
        wids.exif.save.signal_clicked().connect([this]() { on_info_save(); });

        auto* action = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 5);
        auto* spacer = Gtk::make_managed<Gtk::Label>();
        spacer->set_hexpand(true);
        action->append(wids.exif.close);
        action->append(*spacer);
        action->append(wids.exif.reset);
        action->append(wids.exif.save);

        auto* sidebar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 5);
        sidebar->set_name("sidebar");
        sidebar->set_size_request(600);
        sidebar->add_css_class("sidebar");
        sidebar->set_hexpand_set(true);
        sidebar->append(*scrolled);
        sidebar->append(*action);

        // 设置 sidebar revealer - 从右边滑入
        wids.exif.sidebar.set_name("wids.exif.sidebar");
        wids.exif.sidebar.set_child(*sidebar);
        wids.exif.sidebar.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
        wids.exif.sidebar.set_reveal_child(false);
        wids.exif.sidebar.set_hexpand_set(true);

        // 创建遮罩层 Box
        auto gesture = Gtk::GestureClick::create();
        auto* clickable = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        gesture->signal_pressed().connect([this](int n_press, double x, double y) {
            set_sidebar_visible(false);
        });
        clickable->set_name("clickable");
        clickable->set_valign(Gtk::Align::FILL);
        clickable->set_vexpand(true);
        clickable->set_hexpand(true);
        clickable->add_controller(gesture);

        auto* cter = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        cter->append(*clickable);
        cter->append(wids.exif.sidebar);
        // 设置 mask revealer
        // wids.exif.mask.add_css_class("overlay-overlay");
        wids.exif.mask.set_name("wids.exif.mask");
        wids.exif.mask.set_child(*cter);
        wids.exif.mask.set_transition_type(Gtk::RevealerTransitionType::CROSSFADE);
        set_sidebar_visible(false);

        parent->add_css_class("overlay-child");
        wids.overlay.set_name("wids.overlay");
        wids.overlay.add_overlay(wids.exif.mask);
        wids.overlay.set_child(*parent);
    }
    App()
    {
        set_title("libjxl-lite gui");
        set_size_request(DEF_WINDOW_WIDTH, DEF_WINDOW_HEIGHT);

        cancellable = Gio::Cancellable::create();
        provider = Gtk::CssProvider::create();
        provider->load_from_string(R"(
.overlay .overlay-child {
    filter: blur(3px) grayscale(1) opacity(50%);
}
box.sidebar {
    background: #fffc;
    border-left: 1px solid #ccc;
}
.meta-row >.title {
    background-color: #79f3;
    min-height: 50px;
    padding: 0 10px;
    font-size: 1.2em;
}
.meta-row label.value {
    color: #666;
}
button.io-action:hover {
    background: #3584e4;
    color: #fff;
}
button.io-action.busy:hover {
    background: #f6d9d9;
    color: #c30000;
}
button.metadata-dirty {
    box-shadow: inset 0 0 0 2px #f66151;
}
button.metadata-mask,
button.metadata-mask:hover,
button.metadata-mask:active {
    background: rgba(0, 0, 0, 0.45);
    border: none;
    border-radius: 0;
}
box.metadata-sidebar {
    border-right: 1px solid rgba(128, 128, 128, 0.4);
}
box.metadata-section-item {
    background: rgba(128, 128, 128, 0.18);
})");
        Gtk::StyleProvider::add_provider_for_display(Gdk::Display::get_default(), provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        wids.preview.set_name("wids.preview");
        wids.preview.set_valign(Gtk::Align::FILL);
        wids.preview.set_vexpand();
        wids.preview.set_can_shrink(true);
        wids.preview.set_content_fit(Gtk::ContentFit::CONTAIN);

        auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 5);
        root->set_name("root");
        root->append(wids.preview);
        init_animation(root);
        init_control(root);
        init_sidebar(root);
        set_child(wids.overlay);
    }
#pragma endregion // init
};

int main(int argc, char* argv[])
{
#ifdef _WIN32
    setlocale(LC_ALL, ".UTF8");
#endif
    LibraryGuard;
    g_log_set_writer_func([](GLogLevelFlags log_level, const GLogField* fields, gsize n_fields, gpointer usd) {
        const char* domain = nullptr;
        for (gsize i = 0; i < n_fields; i++) {
            if (!strcmp(fields[i].key, "GLIB_DOMAIN")) {
                domain = (const char*)fields[i].value;
                break;
            }
        }
        if (domain && !strcmp(domain, G_LOG_DOMAIN))
            return g_log_writer_default(log_level, fields, n_fields, usd);
        return G_LOG_WRITER_HANDLED;
    },
        nullptr, nullptr);
    auto app = Gtk::Application::create("org.libjxl.lite.gui");
    return app->make_window_and_run<App>(argc, argv);
}
