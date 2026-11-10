#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "jxlw.h"

#pragma region misc
#pragma region misc_log

#define jxlw_critical(message, ...) jxlw_log(JXLW_LOG_LEVEL_CRITICAL, message, ##__VA_ARGS__)
#define jxlw_error(message, ...) jxlw_log(JXLW_LOG_LEVEL_ERROR, message, ##__VA_ARGS__)
#define jxlw_warning(message, ...) jxlw_log(JXLW_LOG_LEVEL_WARNING, message, ##__VA_ARGS__)
#define jxlw_info(message, ...) jxlw_log(JXLW_LOG_LEVEL_INFO, message, ##__VA_ARGS__)
#define jxlw_debug(message, ...) jxlw_log(JXLW_LOG_LEVEL_DEBUG, message, ##__VA_ARGS__)

void jxlw_log(jxlw_log_level_e level, const char* message, ...);

#pragma endregion // misc_log
#pragma region misc_frame

typedef struct _jxlw_frame {
    uint8_t* pixel;
    uint32_t width, height, size;
    jxlw_color_format_e format;
    std::atomic_int_fast32_t ref;
} jxlw_frame_t;

void jxlw_frame_free(jxlw_frame_t* frame);
void jxlw_frame_unref(jxlw_frame_t* frame);
jxlw_frame_t* jxlw_frame_ref(jxlw_frame_t* frame);
// 用一段已经分配好的像素创建一个帧，缓冲区的所有权转移给帧
jxlw_frame_t* jxlw_frame_new_take(jxlw_color_format_e format, const jxlw_frame_args_t* args, uint8_t* buff);
// 复制一段像素创建一个帧
jxlw_frame_t* jxlw_frame_new(jxlw_color_format_e format, const jxlw_frame_args_t* args, const uint8_t* buff);
// 一帧像素的字节数：width * height * 通道数
uint32_t jxlw_frame_calc_bytes(jxlw_frame_t* f);

#pragma endregion // misc_frame
#define jxlw_calc_align_size(size) (((size) + 15) & ~15)

template<typename T>
auto jxlw_malloc_make(size_t size) {
    void* ptr = jxlw_malloc(size);
    if (!ptr) throw std::bad_cast();

    return std::unique_ptr<T, decltype(&jxlw_free)> (static_cast<T*>(ptr), jxlw_free);
}
#pragma endregion // misc
#pragma region backend
#define DEF_BUFF_SIZE 16384

#pragma region backend_reader

typedef struct _jxlw_reader_backend {
    // 读取已经解码好的第 idx 帧，返回的帧需要 jxlw_frame_unref
    jxlw_frame_t* (*read)(struct _jxlw_reader_backend* be, uint32_t idx, jxlw_status_e* err);
    pfn_jxlw_notify notify;
    // 解码之后才能确定的流信息
    jxlw_color_format_e format;
    uint32_t width, height;
    uint32_t frames;
    uint32_t duration; // 动画总时长（ms）
    bool decoded;     // 是否已经解码过
} jxlw_reader_backend_t;

// 探测码流格式，不是这个格式就返回 false（不是错误）
bool jxlw_reader_jxl_probe(const uint8_t* buff, uint32_t size);
bool jxlw_reader_jpeg_probe(const uint8_t* buff, uint32_t size);
//
// 后端是饿汉模式的：创建的时候就把整个码流解码完，
// 之后 read 只是把已经解码好的帧取出来，失败返回 NULL。
jxlw_reader_backend_t* jxlw_reader_jxl_backend_new(const uint8_t* buff, uint32_t size);
jxlw_reader_backend_t* jxlw_reader_jpeg_backend_new(const uint8_t* buff, uint32_t size);

#pragma endregion // backend_reader

#pragma region backend_writer

typedef struct _jxlw_backend_writer {
    // 写入一帧像素，args 只有 width / height 有效
    jxlw_status_e (*write)(struct _jxlw_backend_writer* be, const jxlw_frame_args_t* args, const uint8_t* frame);
    // 结束编码，取出结果（调用方负责释放），结果大小写入 args->size
    uint8_t* (*finish)(struct _jxlw_backend_writer* be, jxlw_frame_args_t* args);
    pfn_jxlw_notify notify;
} jxlw_backend_writer_t;

// using pfn_backend_writer_write = decltype(&jxlw_backend_writer_t::write);
// typedef jxlw_status_e (*pfn_backend_writer_write)(struct _jxlw_backend_writer* be, const uint8_t* frame, const jxlw_frame_args_t* args);

// frames 是将要写入的帧数，用来决定是否编码成动画
jxlw_backend_writer_t* jxlw_writer_jxl_backend_new(const jxlw_writer_args_t* args);
jxlw_backend_writer_t* jxlw_writer_jpeg_backend_new(const jxlw_writer_args_t* args);

#pragma endregion // backend_writer
#pragma endregion // backend
