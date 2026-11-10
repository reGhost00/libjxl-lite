/*
 * JXL backend, built on top of libjxl.
 *
 * Decoding uses the event loop of JxlDecoder (BASIC_INFO / FRAME /
 * FULL_IMAGE), encoding uses JxlEncoder with a growing output buffer.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>


#include <jxl/color_encoding.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
#include <jxl/memory_manager.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/resizable_parallel_runner_cxx.h>

#include "cter.h"
#include "jxlw.h"
#include "inner.h"

#pragma region misc

typedef struct _jxlw_frame_jxl {
    jxlw_frame_t core;
    uint32_t idx;
    uint32_t dur;
} jxlw_frame_jxl_t;

static void jxlw_frame_jxl_unref_wrap(void* it, void* usd) {
    jxlw_frame_unref((jxlw_frame_t*)it);
}

static jxlw_frame_jxl_t* jxlw_frame_jxl_new(uint32_t w, uint32_t h, uint32_t size, jxlw_color_format_e format, uint32_t idx, uint32_t dur) {
    jxlw_frame_jxl_t* f = static_cast<jxlw_frame_jxl_t*>(jxlw_malloc(sizeof(jxlw_frame_jxl_t)));
    if (!f) [[unlikely]] return NULL;
    f->core.pixel = static_cast<uint8_t*>(jxlw_malloc(size));
    if (!f->core.pixel) [[unlikely]] {
        jxlw_free(f);
        return NULL;
    }
    f->core.width = w;
    f->core.height = h;
    f->core.size = size;
    f->core.format = format;
    // 引用计数一定要初始化，否则 unref 的时候行为是未定义的
    std::atomic_init(&f->core.ref, 1);
    f->idx = idx;
    f->dur = dur;
    return f;
}

#pragma endregion // misc
#pragma region reader
typedef struct _jxlw_reader_jxl_backend {
    jxlw_reader_backend_t core;
    c_ptr_array_t* fs;    // 已经解码好的所有帧
} jxlw_reader_jxl_backend_t;

static void* jxlw_jxl_alloc(void* usd, size_t size) {
    return jxlw_malloc(size);
}

static void jxlw_jxl_free(void* usd, void* ptr) {
    jxlw_free(ptr);
}

/**
 * Decode the whole stream.
 *
 * Every frame of the stream is decoded at once and kept in `fs`, so that
 * jxlw_reader_read_frame() can hand them out one by one.
 */
static jxlw_status_e jxlw_reader_jxl_decode(jxlw_reader_jxl_backend_t* jbe, const uint8_t* buff, uint32_t size) {
    JxlMemoryManager mm = {
        .alloc = jxlw_jxl_alloc,
        .free = jxlw_jxl_free
    };

    JxlDecoderPtr dec = JxlDecoderMake(&mm);
    JxlResizableParallelRunnerPtr runner = JxlResizableParallelRunnerMake(&mm);
    if (!dec || !runner) [[unlikely]] {
        jxlw_error("jxlw: failed to create the jxl decoder");
        return JXLW_STATUS_ERR_OUT_OF_MEMORY;
    }
    if (!JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE)) [[likely]] {
        if (!JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get())) [[likely]]
            goto step2;
        jxlw_error("jxlw: failed to init jxl decoder");
        return JXLW_STATUS_ERR_OPERATION_FAILED;
    }
step2:
    JxlBasicInfo info = {};
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8 };
    uint32_t dur = 0;     // 最后一帧的时长
    uint32_t total = 0;   // 所有帧的时长之和
    // 动画的帧时长是 tick，要按照 tps 换算成毫秒
    uint32_t tps_num = 1000, tps_den = 1;
    JxlDecoderStatus st = JxlDecoderSetInput(dec.get(), buff, size);
    if (JXL_DEC_SUCCESS != st) [[unlikely]] {
        jxlw_error("jxlw: failed to set the input of the jxl decoder");
        return JXLW_STATUS_ERR_DECODE_FAILED;
    }
    jxlw_status_e rev = JXLW_STATUS_ERR_DECODE_FAILED;
    while (true) {
        st = JxlDecoderProcessInput(dec.get());
        if (JXL_DEC_BASIC_INFO == st) {
            // GetBasicInfo 成功的时候返回 JXL_DEC_SUCCESS
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(dec.get(), &info)) [[unlikely]] {
                jxlw_error("jxlw: failed to get the basic info");
                break;
            }
            JxlResizableParallelRunnerSetThreads(runner.get(), JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
            if (info.have_animation && info.animation.tps_numerator) {
                tps_num = info.animation.tps_numerator;
                tps_den = info.animation.tps_denominator ? info.animation.tps_denominator : 1;
            }

            format.num_channels = (uint32_t)((bool)info.alpha_bits) + info.num_color_channels;
            if (format.num_channels)
                jbe->core.format = (jxlw_color_format_e)(format.num_channels - 1);
            else
                jbe->core.format = JXLW_COLOR_FORMAT_GRAY;

            jbe->core.width = info.xsize;
            jbe->core.height = info.ysize;
            continue;
        }
        if (JXL_DEC_FRAME == st) {
            size_t fs = 0;
            if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(dec.get(), &format, &fs)) [[unlikely]] {
                jxlw_error("jxlw: failed to decode jxl");
                break;
            }
            if (UINT32_MAX < fs) [[unlikely]] {
                jxlw_error("jxlw: output data out of range");
                rev = JXLW_STATUS_ERR_OUT_OF_MEMORY;
                break;
            }
            JxlFrameHeader header = {};
            if (JXL_DEC_SUCCESS == JxlDecoderGetFrameHeader(dec.get(), &header)) {
                dur = (uint32_t)(((uint64_t)header.duration * 1000 * tps_den) / tps_num);
                total += dur;
            }

            jxlw_frame_jxl_t* f = jxlw_frame_jxl_new(info.xsize, info.ysize, (uint32_t)fs, jbe->core.format, jbe->fs->len, dur);
            if (!f) [[unlikely]] {
                jxlw_error("jxlw: out of memory");
                rev = JXLW_STATUS_ERR_OUT_OF_MEMORY;
                break;
            }
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(dec.get(), &format, f->core.pixel, fs)) [[unlikely]] {
                jxlw_error("jxlw: failed to read decode data");
                jxlw_frame_free((jxlw_frame_t*)f);
                break;
            }
            c_ptr_array_push(jbe->fs, f);
            continue;
        }
        if (JXL_DEC_SUCCESS == st) {
            jbe->core.frames = jbe->fs->len;
            jbe->core.duration = 1 < jbe->fs->len ? total : 0;
            return JXLW_STATUS_OK;
        }
        if (JXL_DEC_ERROR == st || JXL_DEC_NEED_MORE_INPUT == st || JXL_DEC_NEED_IMAGE_OUT_BUFFER == st) break;
    }
    jxlw_error("jxlw: failed to decode the jxl stream");
    return rev;
}

// 帧已经解码好了，这里只是取出来
static jxlw_frame_t* jxlw_reader_jxl_read(jxlw_reader_backend_t* be, uint32_t idx, jxlw_status_e* err) {
    jxlw_reader_jxl_backend_t* jbe = (jxlw_reader_jxl_backend_t*) be;
    if (!be) [[unlikely]] {
        if (err) *err = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }
    if (idx < jbe->fs->len) {
        jxlw_frame_t* f = (jxlw_frame_t*)c_ptr_array_index(jbe->fs, idx);
        return jxlw_frame_ref(f);
    }
    if (err) *err = JXLW_STATUS_ERR_OUT_OF_RANGE;
    return NULL;
}

static void jxlw_reader_jxl_backend_free(void* be) {
    jxlw_reader_jxl_backend_t* jbe = (jxlw_reader_jxl_backend_t*) be;
    if (!jbe) return;
    c_ptr_array_free(jbe->fs, true);
    jxlw_free(be);
}

jxlw_reader_backend_t* jxlw_reader_jxl_backend_new(const uint8_t* buff, uint32_t size) {
    if (!buff || 2 > size) [[unlikely]] return NULL;
    JxlSignature sig = JxlSignatureCheck(buff, size);
    if (JXL_SIG_CODESTREAM != sig && JXL_SIG_CONTAINER != sig) [[unlikely]] return NULL;

    jxlw_reader_jxl_backend_t* jbe = (jxlw_reader_jxl_backend_t*)jxlw_malloc(sizeof(jxlw_reader_jxl_backend_t));
    jbe->fs = c_ptr_array_new_full(1, jxlw_frame_jxl_unref_wrap, NULL);

    // 饿汉模式：整个码流在这里就解码完了
    if (!jxlw_reader_jxl_decode(jbe, buff, size)) [[unlikely]] {
        jbe->core.read = jxlw_reader_jxl_read;
        jbe->core.notify = jxlw_reader_jxl_backend_free;
        return (jxlw_reader_backend_t*)jbe;    
    }

    jxlw_reader_jxl_backend_free(jbe);
    return NULL;
}

#pragma endregion // reader
#pragma region writer

typedef struct _jxlw_writer_jxl_backend {
    jxlw_backend_writer_t core;
    jxlw_writer_args_t args;
    JxlEncoder* enc;
    JxlEncoderFrameSettings* st;
    void* runner;
    uint32_t width, height;

} jxlw_writer_jxl_backend_t;

static void jxlw_writer_jxl_backend_free(void* be) {
    jxlw_writer_jxl_backend_t* jbe = (jxlw_writer_jxl_backend_t*)be;
    if (!jbe) return;
    if (jbe->enc) JxlEncoderDestroy(jbe->enc);
    if (jbe->runner) JxlResizableParallelRunnerDestroy(jbe->runner);
    jxlw_free(be);
}

static jxlw_status_e jxlw_writer_jxl_default_write(jxlw_backend_writer_t* be, const jxlw_frame_args_t* args, const uint8_t* frame) {
    jxlw_error("jxlw: null jxl write operate");
    return JXLW_STATUS_ERR_OPERATION_FAILED;
}

static uint8_t* jxlw_writer_jxl_default_finish(jxlw_backend_writer_t* be, jxlw_frame_args_t* args) {
    jxlw_error("jxlw: null jxl finish operate");
    return NULL;
}

static uint8_t* jxlw_writer_jxl_finish(jxlw_backend_writer_t* be, jxlw_frame_args_t* args) {
    jxlw_writer_jxl_backend_t* jbe = (jxlw_writer_jxl_backend_t*)be;
    if (!jbe->enc) [[unlikely]] return NULL;
    JxlEncoderCloseInput(jbe->enc);

    size_t total = 0;    // 已经写出来的字节数
    size_t avail = DEF_BUFF_SIZE;
    uint8_t* buff = static_cast<uint8_t*>(jxlw_malloc(DEF_BUFF_SIZE));
    if (!buff) [[unlikely]] return NULL;
    while (true) {
        uint8_t* out = buff + total;
        JxlEncoderStatus st = JxlEncoderProcessOutput(jbe->enc, &out, &avail);
        // out 会被移动到下一个空的位置
        total = (size_t)(out - buff);
        if (JXL_ENC_NEED_MORE_OUTPUT == st) {
            size_t size = (total + avail) * 2;
            if (size < DEF_BUFF_SIZE) size = DEF_BUFF_SIZE;
            uint8_t* next = static_cast<uint8_t*>(jxlw_realloc(buff, size));
            if (next) {
                buff = next;
                avail = size - total;
                continue;
            }
            jxlw_error("jxlw: memory alloc failed where finish");
            break;
        }
        if (JXL_ENC_SUCCESS == st) {
            args->width = jbe->width;
            args->height = jbe->height;
            args->size = (uint32_t)total;
            jbe->core.write = jxlw_writer_jxl_default_write;
            jbe->core.finish = jxlw_writer_jxl_default_finish;
            return buff;
        }
        jxlw_error("jxlw: failed to write frame");
        break;
    }
    jxlw_free(buff);
    return NULL;
}

static jxlw_status_e jxlw_writer_jxl_write_working(jxlw_backend_writer_t* be, const jxlw_frame_args_t* args, const uint8_t* frame) {
    jxlw_writer_jxl_backend_t* jbe = (jxlw_writer_jxl_backend_t*)be;
    if (args->width != jbe->width || args->height != jbe->height) [[unlikely]]
        return JXLW_STATUS_ERR_BAD_ARGUMENT;
    
    JxlPixelFormat format = { 1 + jbe->args.format, JXL_TYPE_UINT8 };
    uint64_t size = std::min(args->size, args->width * args->height * format.num_channels);
    if (UINT32_MAX < size) [[unlikely]] return JXLW_STATUS_ERR_OUT_OF_RANGE;
    if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(jbe->st, &format, frame, size)) [[unlikely]] {
        jxlw_error("jxlw: failed to add jxl image frame");
        return JXLW_STATUS_ERR_ENCODE_FAILED;
    }
    return JXLW_STATUS_OK;
}

static jxlw_status_e jxlw_writer_jxl_write_prepare(jxlw_backend_writer_t* be, const jxlw_frame_args_t* args, const uint8_t* frame) {
    jxlw_writer_jxl_backend_t* jbe = (jxlw_writer_jxl_backend_t*)be;
    JxlBasicInfo info = {
        .xsize = args->width,
        .ysize = args->height,
        .bits_per_sample = 8,
        .uses_original_profile = !jbe->args.quality,
        .orientation = JXL_ORIENT_IDENTITY,
    };
    int gray = JXL_FALSE;
    if (JXLW_COLOR_FORMAT_GRAYA == jbe->args.format || JXLW_COLOR_FORMAT_RGBA == jbe->args.format) {
        if (JXLW_COLOR_FORMAT_GRAYA == jbe->args.format) {
            info.num_color_channels = 1;
            gray = JXL_TRUE;
        }
        else
            info.num_color_channels = 3;
        info.num_extra_channels = 1;
        info.alpha_bits = 8;
    }
    else if (JXLW_COLOR_FORMAT_GRAY == jbe->args.format) {
        info.num_color_channels = 1;
        gray = JXL_TRUE;
    }
    else
        info.num_color_channels = 3;
    if (jbe->args.duration) {
        info.have_animation = JXL_TRUE;
        info.animation.tps_numerator = 1000;
        info.animation.tps_denominator = 1;
    }
    if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(jbe->enc, &info)) [[unlikely]] {
        jxlw_error("jxlw: failed to set jxl basic info");
        return JXLW_STATUS_ERR_ENCODE_FAILED;
    }

    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, gray);
    if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(jbe->enc, &color)) [[unlikely]] {
        jxlw_error("jxlw: failed to set jxl color encoding");
        return JXLW_STATUS_ERR_ENCODE_FAILED;
    }

    jbe->st = JxlEncoderFrameSettingsCreate(jbe->enc, nullptr);
    if (!jbe->st) [[unlikely]] {
        jxlw_error("jxlw: failed to create jxl frame settings");
        return JXLW_STATUS_ERR_ENCODE_FAILED;
    }

    if (!jbe->args.quality) {
        if (JXL_ENC_SUCCESS != JxlEncoderSetFrameLossless(jbe->st, JXL_TRUE)) [[unlikely]] {
            jxlw_error("jxlw: failed to set the lossless mode");
            return JXLW_STATUS_ERR_ENCODE_FAILED;
        }
        JxlEncoderSetFrameDistance(jbe->st, 0.0);
        JxlEncoderFrameSettingsSetOption(jbe->st, JXL_ENC_FRAME_SETTING_MODULAR, 1);
        JxlEncoderFrameSettingsSetOption(jbe->st, JXL_ENC_FRAME_SETTING_EFFORT, 9);
    }
    else {
        float distance = 0.5f + (jbe->args.quality - JXLW_WRITER_QUALITY_LOWEST) * 4.5f / (float)JXLW_WRITER_QUALITY_HEIGHT;
        uint32_t effort = 3 + (jbe->args.quality - JXLW_WRITER_QUALITY_LOWEST) * 6 / JXLW_WRITER_QUALITY_HEIGHT;
        distance = std::clamp(distance, 0.5f, 5.0f);
        effort = std::clamp(effort, 3u, 9u);
        JxlEncoderSetFrameDistance(jbe->st, distance);
        // 只有带 alpha 的时候才有额外的通道
        if (info.num_extra_channels)
            JxlEncoderSetExtraChannelDistance(jbe->st, 0, distance);
        JxlEncoderFrameSettingsSetOption(jbe->st, JXL_ENC_FRAME_SETTING_EFFORT, effort);
    }

    if (jbe->args.duration) {
        JxlFrameHeader header = {};
        // InitFrameHeader 会把所有的字段都重置，所以要先初始化再设时长
        JxlEncoderInitFrameHeader(&header);
        header.duration = jbe->args.duration;
        if (JXL_ENC_SUCCESS != JxlEncoderSetFrameHeader(jbe->st, &header)) [[unlikely]] {
            jxlw_error("jxlw: failed to set the jxl frame header");
            return JXLW_STATUS_ERR_ENCODE_FAILED;
        }
    }

    jbe->width = args->width;
    jbe->height = args->height;
    jbe->core.write = jxlw_writer_jxl_write_working;
    jbe->core.finish = jxlw_writer_jxl_finish;
    return jxlw_writer_jxl_write_working(be, args, frame);
}


jxlw_backend_writer_t* jxlw_writer_jxl_backend_new(const jxlw_writer_args_t* args) {
    if (!args) [[unlikely]] return NULL;

    JxlMemoryManager mm = {
        .alloc = jxlw_jxl_alloc,
        .free = jxlw_jxl_free
    };
    jxlw_writer_jxl_backend_t* jbe = (jxlw_writer_jxl_backend_t*)jxlw_malloc(sizeof(jxlw_writer_jxl_backend_t));
    if (!jbe) [[unlikely]] return NULL;
    std::memset(jbe, 0, sizeof(jxlw_writer_jxl_backend_t));
    std::memcpy(&jbe->args, args, sizeof(jxlw_writer_args_t));
    jbe->enc = JxlEncoderCreate(&mm);
    jbe->runner = JxlResizableParallelRunnerCreate(&mm);
    if (jbe->enc && jbe->runner) {
        if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(jbe->enc, JxlResizableParallelRunner, jbe->runner)) [[unlikely]] {
            jxlw_error("jxlw: failed to set the parallel runner");
            jxlw_writer_jxl_backend_free(jbe);
            return NULL;
        }
        jbe->core.write = jxlw_writer_jxl_write_prepare;
        jbe->core.finish = jxlw_writer_jxl_default_finish;
        jbe->core.notify = jxlw_writer_jxl_backend_free;
        return (jxlw_backend_writer_t*)jbe;
    }
    jxlw_writer_jxl_backend_free(jbe);
    return NULL;
}

#pragma endregion // writer
