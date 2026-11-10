/*
 * JPEG backend, built on top of lib/jpegli.
 *
 * jpegli is a from scratch JPEG implementation that shares the techniques of
 * libjxl (but is a completely independent codec).
 *
 * IMPORTANT: jpegli's default error handler calls exit(EXIT_FAILURE), so the
 * error manager is always replaced by a setjmp / longjmp based one. A corrupt
 * file can therefore never kill the host process.
 *
 * The layout of this file mirrors the JXL backend (jxlimg_jxl.cc):
 *   - a "probe" function that recognizes the format,
 *   - an eager reader: the whole picture is decoded when the reader is created,
 *   - an eager writer: the picture is encoded when the writer is released.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "lib/jpegli/common.h"
#include "lib/jpegli/decode.h"
#include "lib/jpegli/encode.h"

#include "jxlw.h"
#include "inner.h"

#pragma region misc
// 一次读写多少行，16 行是 2x2 色度抽样的时候一个 iMCU 的高度
#define JXLW_JPEG_ROWS 16

/**
 * The error manager of jpegli.
 *
 * The default one calls exit(), the messages are collected here and the
 * control is given back to the caller with a longjmp.
 */
typedef struct _jxlw_jpeg_error_mgr {
    struct jpeg_error_mgr pub;
    char message[JMSG_LENGTH_MAX];
    jmp_buf jmp;
} jxlw_jpeg_error_mgr_t;

static void jxlw_jpeg_error_exit(j_common_ptr cinfo) {
    jxlw_jpeg_error_mgr_t* err = (jxlw_jpeg_error_mgr_t*)cinfo->err;
    (*cinfo->err->format_message)(cinfo, err->message);
    err->message[JMSG_LENGTH_MAX - 1] = '\0';
    longjmp(err->jmp, 1);
}

static void jxlw_jpeg_output_message(j_common_ptr cinfo) {
    // 错误消息由 jxlw_jpeg_error_exit 统一打印，这里不要再打一遍
    (void)cinfo;
}

/**
 * Install our error manager into a jpegli object.
 *
 * It has to be called before jpegli_create_*(), because the creation itself
 * can fail (and then the error handler is used).
 */
static void jxlw_jpeg_set_error_mgr(jxlw_jpeg_error_mgr_t* jerr, struct jpeg_error_mgr** slot) {
    *slot = jpegli_std_error(&jerr->pub);
    jerr->pub.error_exit = jxlw_jpeg_error_exit;
    jerr->pub.output_message = jxlw_jpeg_output_message;
}

#pragma endregion // misc
#pragma region reader

typedef struct _jxlw_reader_jpeg_backend {
    jxlw_reader_backend_t core;
    jxlw_jpeg_error_mgr_t jerr;
    struct jpeg_decompress_struct cinfo;
    jxlw_frame_t* frame;
} jxlw_reader_jpeg_backend_t;


static void jxlw_reader_jpeg_backend_free_default(void* usd) {
    if (!usd) [[unlikely]] return;
    jxlw_free(usd);
}

static void jxlw_reader_jpeg_backend_free_padding(void* usd) {
    if (!usd) [[unlikely]] return;
    jxlw_reader_jpeg_backend_t* jbe = (jxlw_reader_jpeg_backend_t*) usd;
    jpegli_destroy_decompress(&jbe->cinfo);
    jxlw_free(usd);
}

static void jxlw_reader_jpeg_backend_free_ready(void* usd) {
    if (!usd) [[unlikely]] return;
    jxlw_reader_jpeg_backend_t* jbe = (jxlw_reader_jpeg_backend_t*) usd;
    jpegli_destroy_decompress(&jbe->cinfo);
    jxlw_frame_unref(jbe->frame);
    jxlw_free(usd);
}

static jxlw_status_e jxlw_reader_jpeg_decode(jxlw_reader_jpeg_backend_t* jbe, const uint8_t* buff, uint32_t size) {
    if (setjmp(jbe->jerr.jmp)) {
        jxlw_error("jpegli: %s", jbe->jerr.message);
        return JXLW_STATUS_ERR_DECODE_FAILED;
    }

    jxlw_jpeg_set_error_mgr(&jbe->jerr, &jbe->cinfo.err);
    jpegli_create_decompress(&jbe->cinfo);
    jbe->core.notify = jxlw_reader_jpeg_backend_free_padding;

    jpegli_mem_src(&jbe->cinfo, buff, size);
    if (JPEG_HEADER_OK != jpegli_read_header(&jbe->cinfo, true)) [[unlikely]]
        return JXLW_STATUS_ERR_DECODE_FAILED;

    // JPEG 只有灰度和 YCbCr 两种，按源格式输出
    jxlw_color_format_e ff = JXLW_COLOR_FORMAT_GRAY;
    jbe->cinfo.out_color_space = JCS_GRAYSCALE;
    if (1 < jbe->cinfo.num_components) {
        if (3 != jbe->cinfo.num_components) {
            jxlw_error("jpegli: unsupported number of components: %d", jbe->cinfo.num_components);
            return JXLW_STATUS_ERR_UNSUPPORTED;
        }
        jbe->cinfo.out_color_space = JCS_RGB;
        ff = JXLW_COLOR_FORMAT_RGB;
    }
    
    if (!(jpegli_start_decompress(&jbe->cinfo) && jbe->cinfo.output_width && jbe->cinfo.output_height)) [[unlikely]]
        return JXLW_STATUS_ERR_DECODE_FAILED;

    // 输出的通道数：灰度是 1 个，彩色是 3 个
    const uint32_t comps = jbe->cinfo.out_color_components;
    const uint64_t need = (uint64_t)jbe->cinfo.output_width * (uint64_t)jbe->cinfo.output_height * (uint64_t)comps;
    if (UINT32_MAX < need) [[unlikely]] return JXLW_STATUS_ERR_OUT_OF_RANGE;

    uint8_t* pixel = static_cast<uint8_t*>(jxlw_malloc(need));
    if (!pixel) [[unlikely]] return JXLW_STATUS_ERR_OUT_OF_MEMORY;
    jxlw_frame_args_t fargs = { jbe->cinfo.output_width, jbe->cinfo.output_height, (uint32_t)need };
    jxlw_frame_t* f = jxlw_frame_new_take(ff, &fargs, pixel);
    if (!f) [[unlikely]] {
        jxlw_free(pixel);
        return JXLW_STATUS_ERR_OUT_OF_MEMORY;
    }

    // 一次读多行，和写入的时候一样
    const size_t stride = (size_t)jbe->cinfo.output_width * comps;
    JSAMPROW rows[JXLW_JPEG_ROWS];
    while (jbe->cinfo.output_scanline < jbe->cinfo.output_height) {
        JDIMENSION remaining = jbe->cinfo.output_height - jbe->cinfo.output_scanline;
        JDIMENSION num = remaining > JXLW_JPEG_ROWS ? JXLW_JPEG_ROWS : remaining;
        for (JDIMENSION i = 0; i < num; i++) {
            rows[i] = (JSAMPROW)(pixel + (size_t)(jbe->cinfo.output_scanline + i) * stride);
        }
        // 解码器可能一次给不满，返回的行数才是真正写出来的行数
        if (!jpegli_read_scanlines(&jbe->cinfo, rows, num)) [[unlikely]] {
            jpegli_finish_decompress(&jbe->cinfo);
            jxlw_frame_unref(f);
            return JXLW_STATUS_ERR_DECODE_FAILED;
        }
    }
    jpegli_finish_decompress(&jbe->cinfo);

    jbe->frame = f;
    jbe->core.format = ff;
    jbe->core.width = jbe->cinfo.output_width;
    jbe->core.height = jbe->cinfo.output_height;
    jbe->core.frames = 1;
    jbe->core.duration = 0;
    jbe->core.notify = jxlw_reader_jpeg_backend_free_ready;
    return JXLW_STATUS_OK;
}


// 帧已经解码好了，这里只是取出来
static jxlw_frame_t* jxlw_reader_jpeg_read(jxlw_reader_backend_t* be, uint32_t idx, jxlw_status_e* err) {
    jxlw_reader_jpeg_backend_t* jbe = (jxlw_reader_jpeg_backend_t*)be;
    if (!be) [[unlikely]] {
        if (err) *err = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }
    if (!jbe->frame) [[unlikely]] {
        if (err) *err = JXLW_STATUS_ERR_OPERATION_FAILED;
        return NULL;
    }
    // jpeg 只有一张图，idx 会被忽略
    return jxlw_frame_ref(jbe->frame);
}


jxlw_reader_backend_t* jxlw_reader_jpeg_backend_new(const uint8_t* buff, uint32_t size) {
    if (!buff || 2 > size) [[unlikely]] return NULL;
    if (0xFF != buff[0] || 0xD8 != buff[1]) return NULL;

    jxlw_reader_jpeg_backend_t* jbe = static_cast<jxlw_reader_jpeg_backend_t*>(jxlw_malloc(sizeof(jxlw_reader_jpeg_backend_t)));
    if (!jbe) [[unlikely]] return NULL;
    std::memset(jbe, 0, sizeof(jxlw_reader_jpeg_backend_t));
    jbe->core.notify = jxlw_reader_jpeg_backend_free_default;
    // 饿汉模式：整个码流在这里就解码完了
    if (!jxlw_reader_jpeg_decode(jbe, buff, size)) [[likely]] {
        jbe->core.read = jxlw_reader_jpeg_read;
        return (jxlw_reader_backend_t*)jbe;    
    }
    jbe->core.notify(jbe);
    return NULL;
}

#pragma endregion // reader
#pragma region writer

typedef struct _jxlw_writer_jpeg_backend {
    jxlw_backend_writer_t core;
    jxlw_writer_args_t args;
    jxlw_jpeg_error_mgr_t jerr;
    struct jpeg_compress_struct cinfo;
    uint32_t width, height, size;
    uint8_t* dst;
    // 源数据和 jpeg 的通道数不一样的时候（去掉 alpha 通道）用的行缓冲
    uint8_t* pack;
    size_t pack_size;
} jxlw_writer_jpeg_backend_t;

static void jxlw_writer_jpeg_backend_free_default(void* usd) {
    if (!usd) [[unlikely]] return;
    jxlw_writer_jpeg_backend_t* jbe = (jxlw_writer_jpeg_backend_t*)usd;
    jpegli_destroy_compress(&jbe->cinfo);
    jxlw_free(jbe->pack);
    jxlw_free(jbe);
}

static void jxlw_writer_jpeg_backend_free_ready(void* usd) {
    if (!usd) [[unlikely]] return;
    jxlw_writer_jpeg_backend_t* jbe = (jxlw_writer_jpeg_backend_t*)usd;
    jxlw_free(jbe->dst);
    jxlw_writer_jpeg_backend_free_default(usd);
}

static uint8_t* jxlw_writer_jpeg_finish_done(jxlw_backend_writer_t* be, jxlw_frame_args_t* args) {
    jxlw_writer_jpeg_backend_t* jbe = (jxlw_writer_jpeg_backend_t*)be;
    if (jbe->dst && jbe->size) {
        uint8_t* pix = (uint8_t*)jxlw_malloc(jbe->size);
        if (!pix) [[unlikely]] return NULL;
        std::memcpy(pix, jbe->dst, jbe->size);
        args->width = jbe->width;
        args->height = jbe->height;
        args->size = jbe->size;
        return pix;
    }
    return NULL;
}

static uint8_t* jxlw_writer_jpeg_finish_padding(jxlw_backend_writer_t* be, jxlw_frame_args_t* args) {
    jxlw_error("jxlw: operator not available\n");
    return NULL;
}

static jxlw_status_e jxlw_writer_jpeg_write_done(jxlw_backend_writer_t* be, const jxlw_frame_args_t* args, const uint8_t* frame) {
    jxlw_error("jxlw: jpeg does not support multi frames\n");
    return JXLW_STATUS_ERR_UNSUPPORTED;
}

static jxlw_status_e jxlw_writer_jpeg_write_working(jxlw_backend_writer_t* be, const jxlw_frame_args_t* args, const uint8_t* frame) {
    static const uint32_t def_quality_map[] = { 95, 25, 50, 75, 85, 95 };
    jxlw_writer_jpeg_backend_t* jbe = (jxlw_writer_jpeg_backend_t*)be;
    if (!(be && frame && args)) [[unlikely]] return JXLW_STATUS_ERR_BAD_ARGUMENT;
    if (!args->width || !args->height) [[unlikely]] return JXLW_STATUS_ERR_BAD_ARGUMENT;

    // 源数据的通道数：gray=1，gray+alpha=2，rgb=3，rgb+alpha=4
    const uint32_t src_channels = 1u + (uint32_t)jbe->args.format;
    // jpeg 只有灰度和 YCbCr 两种，alpha 通道要丢掉
    const bool gray = JXLW_COLOR_FORMAT_RGB > jbe->args.format;
    const uint32_t dst_channels = gray ? 1u : 3u;

    jbe->cinfo.image_width = args->width;
    jbe->cinfo.image_height = args->height;
    jbe->cinfo.input_components = dst_channels;
    jbe->cinfo.in_color_space = gray ? JCS_GRAYSCALE : JCS_RGB;

    const uint64_t need = (uint64_t)args->width * (uint64_t)args->height * (uint64_t)src_channels;
    if (UINT32_MAX < need || args->size < need) [[unlikely]] return JXLW_STATUS_ERR_BAD_ARGUMENT;

    if (setjmp(jbe->jerr.jmp)) {
        jxlw_error("jpegli: %s", jbe->jerr.message);
        return JXLW_STATUS_ERR_ENCODE_FAILED;
    }

    uint8_t* buff = NULL;
    unsigned long out_size = 0;
    const size_t src_stride = (size_t)args->width * src_channels;
    const size_t dst_stride = (size_t)args->width * dst_channels;
    JSAMPROW rows[JXLW_JPEG_ROWS];
    jbe->width = args->width;
    jbe->height = args->height;
    jpegli_set_defaults(&jbe->cinfo);
    jpegli_set_quality(&jbe->cinfo, def_quality_map[jbe->args.quality], 1);
    jpegli_mem_dest(&jbe->cinfo, &buff, &out_size);
    jpegli_start_compress(&jbe->cinfo, 1);

    // 源数据带 alpha 的时候，要先把颜色通道挑出来，jpegli 只吃没有 alpha 的行
    if (src_channels != dst_channels) {
        jxlw_free(jbe->pack);
        jbe->pack = NULL;
        jbe->pack_size = (size_t)JXLW_JPEG_ROWS * dst_stride;
        jbe->pack = (uint8_t*)jxlw_malloc(jbe->pack_size);
        if (!jbe->pack) [[unlikely]] return JXLW_STATUS_ERR_OUT_OF_MEMORY;
    }

    while (jbe->cinfo.next_scanline < jbe->cinfo.image_height) {
        JDIMENSION remaining = jbe->cinfo.image_height - jbe->cinfo.next_scanline;
        JDIMENSION num = remaining > JXLW_JPEG_ROWS ? JXLW_JPEG_ROWS : remaining;
        // 行的起点是 next_scanline，jpegli 会按照这个偏移来取数据
        const uint8_t* src = frame + (size_t)jbe->cinfo.next_scanline * src_stride;
        if (jbe->pack) {
            for (JDIMENSION i = 0; i < num; i++) {
                const uint8_t* s = src + (size_t)i * src_stride;
                uint8_t* d = jbe->pack + (size_t)i * dst_stride;
                if (gray) {
                    for (uint32_t x = 0; x < args->width; x++) {
                        d[x] = s[(size_t)x * src_channels];
                    }
                }
                else {
                    for (uint32_t x = 0; x < args->width; x++) {
                        const uint8_t* p = s + (size_t)x * src_channels;
                        d[x * 3 + 0] = p[0];
                        d[x * 3 + 1] = p[1];
                        d[x * 3 + 2] = p[2];
                    }
                }
            }
            for (JDIMENSION i = 0; i < num; i++) {
                rows[i] = (JSAMPROW)(jbe->pack + (size_t)i * dst_stride);
            }
        }
        else {
            for (JDIMENSION i = 0; i < num; i++) {
                rows[i] = (JSAMPROW)(src + (size_t)i * src_stride);
            }
        }
        const JDIMENSION prev = jbe->cinfo.next_scanline;
        jpegli_write_scanlines(&jbe->cinfo, rows, num);
        // 编码器没有吃掉任何一行的话，继续循环也没有意义
        if (prev == jbe->cinfo.next_scanline) [[unlikely]] {
            jxlw_error("jpegli: the encoder did not consume any row");
            return JXLW_STATUS_ERR_ENCODE_FAILED;
        }
    }
    jpegli_finish_compress(&jbe->cinfo);

    jbe->size = (uint32_t)out_size;
    if (buff && jbe->size) {
        jbe->dst = (uint8_t*)jxlw_malloc(jbe->size);
        if (!jbe->dst) [[unlikely]] {
            std::free(buff);
            return JXLW_STATUS_ERR_OUT_OF_MEMORY;
        }
        std::memcpy(jbe->dst, buff, jbe->size);
        std::free(buff);
    }

    jbe->core.write = jxlw_writer_jpeg_write_done;
    jbe->core.finish = jxlw_writer_jpeg_finish_done;
    jbe->core.notify = jxlw_writer_jpeg_backend_free_ready;
    return JXLW_STATUS_OK;
}

jxlw_backend_writer_t* jxlw_writer_jpeg_backend_new(const jxlw_writer_args_t* args) {
    if (!args) [[unlikely]] return NULL;

    jxlw_writer_jpeg_backend_t* jbe = static_cast<jxlw_writer_jpeg_backend_t*>(jxlw_malloc(sizeof(jxlw_writer_jpeg_backend_t)));
    if (!jbe) [[unlikely]] return NULL;
    std::memset(jbe, 0, sizeof(jxlw_writer_jpeg_backend_t));
    if (setjmp(jbe->jerr.jmp)) {
        jxlw_error("jpegli: %s", jbe->jerr.message);
        jpegli_destroy_compress(&jbe->cinfo);
        jxlw_free(jbe);
        return NULL;
    }
    jxlw_jpeg_set_error_mgr(&jbe->jerr, &jbe->cinfo.err);
    jpegli_create_compress(&jbe->cinfo);

    std::memcpy(&jbe->args, args, sizeof(jxlw_writer_args_t));
    jbe->core.write = jxlw_writer_jpeg_write_working;
    jbe->core.finish = jxlw_writer_jpeg_finish_padding;
    jbe->core.notify = jxlw_writer_jpeg_backend_free_default;
    return (jxlw_backend_writer_t*)jbe;
}

#pragma endregion // writer
