/*
 * jxlw - a small unified image codec for JPEG and JPEG XL.
 *
 * The library wraps libjxl (JXL) and lib/jpegli (JPEG) behind one pure C,
 * stb_image like API: the caller does not have to care about which of the two
 * backends is used underneath.
 *
 * All entry points are re-entrant with respect to different objects: two
 * readers (or two writers) may be used concurrently, but a single object must
 * not be used from several threads at the same time.
 *
 * The library never aborts the host process: every failure is reported with a
 * status code (and an optional message through jxlw_last_error()).
 */
#ifndef JXLW_H_
#define JXLW_H_

#if defined(_WIN32) && defined(JXLW_API_STDCALL)
#define JXLW_API __stdcall
#else
#define JXLW_API
#endif
#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif /* __cplusplus */

#pragma region misc
#pragma region misc_allocator

/** Allocate `size` bytes. Returns NULL on failure. */
typedef void* (*pfn_jxlw_malloc)(size_t size);
/** Resize a block previously returned by `malloc`. Returns NULL on failure. */
typedef void* (*pfn_jxlw_realloc)(void* ptr, size_t size);
typedef void (*pfn_jxlw_notify)(void* usd);
typedef bool (*pfn_jxlw_notifyb)(void* usd);

typedef struct _jxlw_allocator {
    pfn_jxlw_malloc     malloc;
    pfn_jxlw_realloc    realloc;
    pfn_jxlw_notify     free;
} jxlw_allocator_t;

/**
 * Replace the library wide default allocator.
 *
 * The allocator table is copied, so the caller may keep it on the stack.
 * The missing entries are replaced by the default ones (malloc / realloc /
 * free and the aligned variants).
 *
 * This has to be called *before* any reader or writer is created and from a
 * single thread: the first call wins, every later call returns false. All the
 * memory of the library (readers, writers, decoded frames, encoded streams)
 * is allocated with it.
 *
 * Note: the memory allocated inside lib/jpegli is *not* controlled by this
 * hook, jpegli always uses the C runtime allocator.
 */
JXLW_API bool jxlw_misc_set_allocator(const jxlw_allocator_t* alloc);
JXLW_API void* jxlw_malloc(size_t size);
JXLW_API void* jxlw_realloc(void* ptr, size_t size);
JXLW_API void jxlw_free(void* ptr);
#pragma endregion // misc_allocator
#pragma region misc_log

typedef enum _jxlw_log_level : uint32_t {
    JXLW_LOG_LEVEL_CRITICAL,
    JXLW_LOG_LEVEL_ERROR,
    JXLW_LOG_LEVEL_WARNING,
    JXLW_LOG_LEVEL_INFO,
    JXLW_LOG_LEVEL_DEBUG,
    JXLW_LOG_LEVEL_CNT
} jxlw_log_level_e;

typedef void (*pfn_jxlw_log_handler)(jxlw_log_level_e level, const char* message, void* usd);
//
// Set a log handler for the library. NULL means the default one (stderr)
JXLW_API void jxlw_set_log_handler(pfn_jxlw_log_handler handler, void* usd);

#pragma endregion // misc_log
#pragma region misc_vfs
typedef enum _jxlw_file_seek : uint32_t {
    JXLW_FILE_SEEK_SET,
    JXLW_FILE_SEEK_CUR,
    JXLW_FILE_SEEK_END
} jxlw_file_seek_e;

/** Read up to `bytes` bytes, returns the number of bytes read (<= 0 on error). */
typedef int32_t (*pfn_jxlw_vfs_read)(uint8_t* buff, uint32_t bytes, void* usd);
/** Write up to `bytes` bytes, returns the number of bytes written (<= 0 on error). */
typedef int32_t (*pfn_jxlw_vfs_write)(const uint8_t* buff, uint32_t bytes, void* usd);
/** Move the cursor, returns the new position or a negative value on error. */
typedef int32_t (*pfn_jxlw_vfs_seek)(int32_t offset, jxlw_file_seek_e whence, void* usd);
/** Current position or a negative value on error. */
typedef int32_t (*pfn_jxlw_vfs_tell)(void* usd);
/** Total size in bytes or a negative value when unknown. */
typedef int32_t (*pfn_jxlw_vfs_query_size)(void* usd);

typedef struct _jxlw_vfs_source {
    pfn_jxlw_notifyb        open;   /* optional */
    pfn_jxlw_vfs_read       read;   /* required */
    pfn_jxlw_vfs_write      write;  /* optional */
    pfn_jxlw_vfs_seek       seek;   /* optional */
    pfn_jxlw_vfs_tell       tell;   /* optional */
    pfn_jxlw_vfs_query_size query;  /* optional */
    pfn_jxlw_notify         close;  /* optional */
} jxlw_vfs_source_t;

#pragma endregion // misc_vfs

typedef enum _jxlw_status : uint32_t {
    JXLW_STATUS_OK,
    JXLW_STATUS_ERR_BAD_ARGUMENT,
    JXLW_STATUS_ERR_DECODE_FAILED,
    JXLW_STATUS_ERR_ENCODE_FAILED,
    JXLW_STATUS_ERR_IO,
    JXLW_STATUS_ERR_OPERATION_FAILED,
    JXLW_STATUS_ERR_OUT_OF_MEMORY,
    JXLW_STATUS_ERR_OUT_OF_RANGE,
    JXLW_STATUS_ERR_UNAVAILABLE,
    JXLW_STATUS_ERR_UNKNOW_FORMAT,
    JXLW_STATUS_ERR_UNSUPPORTED,
    JXLW_STATUS_ERR_UNDEFINED,
    JXLW_STATUS_CNT
} jxlw_status_e;

typedef enum _jxlw_color_format : uint32_t {
    JXLW_COLOR_FORMAT_GRAY,
    JXLW_COLOR_FORMAT_GRAYA,
    JXLW_COLOR_FORMAT_RGB,
    JXLW_COLOR_FORMAT_RGBA,
    JXLW_COLOR_FORMAT_CNT
} jxlw_color_format_e;

typedef enum _jxlw_codec_type : uint32_t {
    JXLW_CODEC_TYPE_UNKNOW,
    JXLW_CODEC_TYPE_JXL,
    JXLW_CODEC_TYPE_JPEG,
    JXLW_CODEC_TYPE_CNT
} jxlw_codec_type_e;

JXLW_API jxlw_status_e jxlw_get_last_error();
/** A short human readable name of a status code, never NULL. */
JXLW_API const char* jxlw_get_status_string(jxlw_status_e status);

#pragma endregion // misc
#pragma region reader

typedef struct _jxlw_reader_args {
    jxlw_color_format_e format;
    jxlw_codec_type_e type;
    uint32_t width, height;
    uint32_t frames;       // number of decoded frames, jpeg is 1
    uint32_t duration;     // animation duration in ms, jpeg is 0
} jxlw_reader_args_t;

// 此结构体兼容 jxlw_frame_t
typedef struct _jxlw_frame_args {
    uint32_t width, height, size;
} jxlw_frame_args_t;

typedef struct _jxlw_reader jxlw_reader_t;

/**
 * Decrement the reference counter, destroys the reader when it reaches 0.
 */
JXLW_API void jxlw_reader_unref(jxlw_reader_t* rd);
/** Increment the reference counter of the reader. */
JXLW_API jxlw_reader_t* jxlw_reader_ref(jxlw_reader_t* rd);
/**
 * Create a reader on top of a user supplied VFS.
 *
 * The reader is eager: the whole stream is read and decoded while it is
 * created, so a failure is reported here and not later. All the frames are
 * kept in memory until the reader is destroyed.
 *
 * `args` may be NULL (the defaults are used). `usd` is passed to the VFS
 * callbacks. The reader takes the ownership of the source: the `close`
 * callback is called when the reader is destroyed.
 *
 * Returns NULL on failure, see jxlw_last_error().
 */
JXLW_API jxlw_reader_t* jxlw_reader_new(const jxlw_vfs_source_t* src, void* usd);
// Create a reader from a file. The path is UTF-8 encoded.
// The file is decoded completely before this function returns.
JXLW_API jxlw_reader_t* jxlw_reader_new_from_file(const char* path);
// Create a reader from a memory buffer. The buffer is copied.
// The buffer is decoded completely before this function returns.
JXLW_API jxlw_reader_t* jxlw_reader_new_from_memory(const uint8_t* buff, uint32_t size);
//
//  jxl:
//  read `idx` frame bitmap and return. fill `args` if available
//  jpeg:
//  read bitmap and return. `idx` will ignore
//  the frame is already decoded, this returns a copy of it,
//  the caller has to release it with jxlw_free()
JXLW_API uint8_t* jxlw_reader_read_frame(jxlw_reader_t* rd, uint32_t idx, jxlw_frame_args_t* args);

// Fill `args` with the information of the opened stream.
JXLW_API bool jxlw_reader_get_args(jxlw_reader_t* rd, jxlw_reader_args_t* args);

#pragma endregion // reader
#pragma region writer
typedef struct _jxlw_writer jxlw_writer_t;
typedef enum _jxlw_writer_quality : uint32_t {
    JXLW_WRITER_QUALITY_LOSSLESS,   // jpeg: equal to HEIGEST
    JXLW_WRITER_QUALITY_LOWEST,
    JXLW_WRITER_QUALITY_LOW,
    JXLW_WRITER_QUALITY_DEFAULT,
    JXLW_WRITER_QUALITY_HEIGHT,
    JXLW_WRITER_QUALITY_HEIGEST,
    JXLW_WRITER_QUALITY_CNT
} jxlw_writer_quality_e;

typedef struct _jxlw_writer_args {
    jxlw_color_format_e format; // the format of the pixels given to jxlw_writer_add_frame()
    jxlw_codec_type_e type; // the format of the produced stream
    jxlw_writer_quality_e quality;
    uint32_t duration;  // the duration of a frame in ms, jxl only
} jxlw_writer_args_t;

/**
 * Create a writer.
 *
 * The writer is eager too, but in the other direction: the frames are
 * collected by jxlw_writer_add_frame() and the whole stream is encoded when
 * the writer is released, see jxlw_writer_unref().
 *
 * `args->format` is the format of the pixels given to jxlw_writer_add_frame(),
 * `args->type` is the format of the produced stream. JPEG only supports gray
 * and rgb, so an alpha channel is dropped when the target is JPEG.
 */
JXLW_API jxlw_writer_t* jxlw_writer_new(const jxlw_writer_args_t* args);

// Increment the reference counter of the writer.
JXLW_API jxlw_writer_t* jxlw_writer_ref(jxlw_writer_t* wr);
//
// call this multi time will output jxl
JXLW_API jxlw_status_e jxlw_writer_add_frame(jxlw_writer_t* wr, jxlw_frame_args_t* args, const uint8_t* buff);

/**
 * Encode the collected frames, write them to `src` and release the writer.
 *
 * The encoding happens at the first call, the result is kept in the writer and
 * reused by the later calls. When `src` is NULL nothing is encoded, the writer
 * is only released.
 *
 * The writer is always destroyed, even when the encoding fails. The `close`
 * callback of `src` is called at the end, the writer takes the ownership of
 * the source.
 *
 * This function may be called several times: only the first call encodes the
 * frames, the result is kept in the writer and reused by the later calls. The
 * writer stays alive as long as the reference counter is not zero, so an extra
 * jxlw_writer_ref() is needed to call this function more than once.
 */
JXLW_API jxlw_status_e jxlw_writer_unref(jxlw_writer_t* wr, jxlw_vfs_source_t* src, void* usd);

// Same as jxlw_writer_unref() with a file as the destination.
JXLW_API jxlw_status_e jxlw_writer_unref_to_file(jxlw_writer_t* wr, const char* path);

/**
 * Same as jxlw_writer_unref() with a memory buffer as the destination.
 *
 * The returned buffer is owned by the caller, it has to be released with
 * jxlw_free(). NULL is returned when the encoding failed.
 */
JXLW_API uint8_t* jxlw_writer_unref_to_bytes(jxlw_writer_t* wr, uint32_t* size);

#pragma endregion // writer
#ifdef __cplusplus
}
#endif

#endif /* JXLW_H_ */
