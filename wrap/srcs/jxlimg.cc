/*
 * jxlw - dispatch layer.
 *
 * Responsibilities:
 *   - the default allocator and the allocator bridge
 *   - the file / memory VFS
 *   - format probing (JPEG vs JXL)
 *   - lifetime management of the readers / writers
 *
 * The dispatch layer never touches the pixels itself, it only routes to the
 * backends and normalizes the error codes.
 */

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cter.h"
#include "inner.h"
#include "jxlw.h"

#pragma region misc

static __thread jxlw_status_e g_last_status = JXLW_STATUS_OK;

JXLW_API jxlw_status_e jxlw_get_last_error() {
    jxlw_status_e st = g_last_status;
    g_last_status = JXLW_STATUS_OK;
    return st;
}
/** A short human readable name of a status code, never NULL. */
JXLW_API const char* jxlw_get_status_string(jxlw_status_e status) {
    switch (status) {
    case JXLW_STATUS_OK: return "ok";
    case JXLW_STATUS_ERR_BAD_ARGUMENT: return "bad argument";
    case JXLW_STATUS_ERR_DECODE_FAILED: return "decode failed";
    case JXLW_STATUS_ERR_ENCODE_FAILED: return "encode failed";
    case JXLW_STATUS_ERR_IO: return "io error";
    case JXLW_STATUS_ERR_OPERATION_FAILED: return "operation failed";
    case JXLW_STATUS_ERR_OUT_OF_MEMORY: return "out of memory";
    case JXLW_STATUS_ERR_OUT_OF_RANGE: return "out of range";
    case JXLW_STATUS_ERR_UNAVAILABLE: return "operation unavailable";
    case JXLW_STATUS_ERR_UNKNOW_FORMAT: return "unknown format";
    case JXLW_STATUS_ERR_UNSUPPORTED: return "operation unsupported";
    // case JXLW_STATUS_ERR_INTERNAL: return "internal error";
    default: break;
    }
    return "unknown error";
}

#pragma region misc_alloc
typedef enum _jxlw_allocator_state : uint32_t {
    E_JXLW_ALLOC_STATE_NONE,
    E_JXLW_ALLOC_STATE_PADDING,
    E_JXLW_ALLOC_STATE_DONE
} jxlw_allocator_state_e;

static std::atomic<jxlw_allocator_state_e> def_alloc_state = E_JXLW_ALLOC_STATE_NONE;
#ifdef JXLW_DEBUG
#include <map>
#include <mutex>

static void* jxlw_default_malloc_wrap(size_t size);
static void* jxlw_default_realloc_wrap(void* ptr, size_t size);
static void jxlw_default_free(void* ptr);
static void* jxlw_tracker_track_with_init(void* p, size_t size);
static decltype(&jxlw_tracker_track_with_init) jxlw_alloc_track = jxlw_tracker_track_with_init;

static jxlw_allocator_t def_allocator = {
    .malloc = jxlw_default_malloc_wrap,
    .realloc = jxlw_default_realloc_wrap,
    .free = jxlw_default_free,
};

static std::map<void*, uint64_t> def_tracker_table;
static std::recursive_mutex def_tracker_mtx;

static void jxlw_tracker_track_leak() {
    uint32_t cnt = def_tracker_table.size();
    if (cnt) {
        uint64_t total = 0;
        for (auto& it : def_tracker_table) {
            total += it.second;
            std::printf("[jxlw] Memory leak: %p %zu bytes\n", it.first, it.second);
            jxlw_free(it.first);
        }
        std::printf("[jxlw] Memory leak: %u blocks %zu bytes\n", (uint32_t)def_tracker_table.size(), total);
    }
}

static void* jxlw_tracker_track(void* p, size_t size) {
    std::lock_guard<std::recursive_mutex> lock(def_tracker_mtx);
    def_tracker_table[p] = size;
    return p;
}

static void* jxlw_tracker_track_with_init(void* p, size_t size)
{
    std::lock_guard<std::recursive_mutex> lock(def_tracker_mtx);
    std::atexit(jxlw_tracker_track_leak);
    jxlw_alloc_track = jxlw_tracker_track;
    return jxlw_tracker_track(p, size);
}

static void* jxlw_default_malloc(size_t size) {
    return jxlw_alloc_track(std::malloc(size), size);
}

static void* jxlw_default_realloc(void* ptr, size_t size) {
    if (!ptr) return jxlw_alloc_track(std::malloc(size), size);
    std::lock_guard<std::recursive_mutex> lock(def_tracker_mtx);
    auto it = def_tracker_table.find(ptr);
    if (it != def_tracker_table.end())
        def_tracker_table.erase(it);
    return jxlw_alloc_track(std::realloc(ptr, size), size);
}

static void jxlw_default_free(void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::recursive_mutex> lock(def_tracker_mtx);
    auto it = def_tracker_table.find(ptr);
    if (it != def_tracker_table.end()) {
        std::free(it->first);
        def_tracker_table.erase(it);
    }
}

static bool jxlw_allocator_set_status() {
    jxlw_allocator_state_e exp = E_JXLW_ALLOC_STATE_NONE;
    if (def_alloc_state.compare_exchange_strong(exp, E_JXLW_ALLOC_STATE_PADDING, std::memory_order_acquire, std::memory_order_relaxed)) {
        def_allocator.malloc = jxlw_default_malloc;
        def_allocator.realloc = jxlw_default_realloc;
        return true;
    }
    return false;
}

static void* jxlw_default_malloc_wrap(size_t size) {
    if (jxlw_allocator_set_status()) {
        void* ptr = jxlw_default_malloc(size);
        def_alloc_state.store(E_JXLW_ALLOC_STATE_DONE, std::memory_order_relaxed);
        return ptr;
    }
    throw std::bad_alloc();
}

static void* jxlw_default_realloc_wrap(void* ptr, size_t size) {
    if (jxlw_allocator_set_status()) {
        ptr = jxlw_default_realloc(ptr, size);
        def_alloc_state.store(E_JXLW_ALLOC_STATE_DONE, std::memory_order_relaxed);
        return ptr;
    }
    throw std::bad_alloc();
}
#else   //  JXLW_DEBUG

static void* jxlw_default_malloc(size_t size);
static void* jxlw_default_realloc(void* ptr, size_t size);

static jxlw_allocator_t def_allocator = {
    .malloc = jxlw_default_malloc,
    .realloc = jxlw_default_realloc,
    .free = std::free,
};

static bool jxlw_allocator_set_status() {
    jxlw_allocator_state_e exp = E_JXLW_ALLOC_STATE_NONE;
    if (def_alloc_state.compare_exchange_strong(exp, E_JXLW_ALLOC_STATE_PADDING, std::memory_order_acquire, std::memory_order_relaxed)) {
        def_allocator.malloc = std::malloc;
        def_allocator.realloc = std::realloc;
        return true;
    }
    return false;
}

static void* jxlw_default_malloc(size_t size) {
    if (jxlw_allocator_set_status()) {
        def_alloc_state.store(E_JXLW_ALLOC_STATE_DONE, std::memory_order_relaxed);
        return std::malloc(size);
    }
    throw std::bad_alloc();
}

static void* jxlw_default_realloc(void* ptr, size_t size) {
    if (jxlw_allocator_set_status()) {
        def_alloc_state.store(E_JXLW_ALLOC_STATE_DONE, std::memory_order_relaxed);
        return std::realloc(ptr, size);
    }
    throw std::bad_alloc();
}

JXLW_API bool jxlw_misc_set_allocator(const jxlw_allocator_t* alloc) {
    jxlw_allocator_state_e exp = E_JXLW_ALLOC_STATE_NONE;
    if (def_alloc_state.compare_exchange_strong(exp, E_JXLW_ALLOC_STATE_DONE, std::memory_order_acquire, std::memory_order_relaxed)) {
        std::memcpy(&def_allocator, alloc, sizeof(jxlw_allocator_t));
        c_set_allocator((const c_allocator_t*)&def_allocator);
        return true;
    }
    return false;
}
#endif
JXLW_API void* jxlw_malloc(size_t size) {
    return def_allocator.malloc(size);
}

JXLW_API void* jxlw_realloc(void* ptr, size_t size) {
    return def_allocator.realloc(ptr, size);
}

JXLW_API void jxlw_free(void* ptr) {
    def_allocator.free(ptr);
}
#pragma endregion // misc_alloc
#pragma region misc_log
static void jxlw_log_default_handler(jxlw_log_level_e level, const char* buffer, void* usd)
{
    if (JXLW_LOG_LEVEL_CRITICAL == level) {
        printf("\x1b[1;41m%s\x1b[0;49m", buffer);
        std::abort();
    }
    if (JXLW_LOG_LEVEL_ERROR == level) {
        printf("\x1b[91;49m%s\x1b[0m", buffer);
        return;
    }
    if (JXLW_LOG_LEVEL_WARNING == level) {
        printf("\x1b[93;49m%s\x1b[0m", buffer);
        return;
    }
    if (JXLW_LOG_LEVEL_INFO == level) {
        printf("\x1b[96;49m%s\x1b[0m", buffer);
        return;
    }
    puts(buffer);
}

static pfn_jxlw_log_handler jxlw_logger = jxlw_log_default_handler;
static void* jxlw_logger_usd = NULL;

void jxlw_log(jxlw_log_level_e level, const char* format, ...)
{
    static char buff[3131];
    va_list args;
    va_start(args, format);

    memset(buff, 0, sizeof(buff));
    vsnprintf(buff, sizeof(buff) - 1, format, args);
    if (!jxlw_logger) [[unlikely]] jxlw_logger = jxlw_log_default_handler;
    jxlw_logger(level, buff, jxlw_logger_usd);
    va_end(args);
}

JXLW_API void jxlw_set_log_handler(pfn_jxlw_log_handler handler, void* usd) {
    jxlw_logger = handler ? handler : jxlw_log_default_handler;
    jxlw_logger_usd = usd;
}

#pragma endregion // misc_log
#pragma region misc_frame

void jxlw_frame_free(jxlw_frame_t* frame) {
    // if (!frame || 1 < std::atomic_fetch_sub_explicit(&frame->ref, 1, std::memory_order_release)) return;
    jxlw_free(frame->pixel);
    jxlw_free(frame);
    // if (frame->notify) frame->notify(frame);
}

void jxlw_frame_unref(jxlw_frame_t* frame) {
    if (!frame || 1 < std::atomic_fetch_sub_explicit(&frame->ref, 1, std::memory_order_release)) return;
    jxlw_frame_free(frame);
}

jxlw_frame_t* jxlw_frame_ref(jxlw_frame_t* frame) {
    std::atomic_fetch_add_explicit(&frame->ref, 1, std::memory_order_relaxed);
    return frame;
}

jxlw_frame_t* jxlw_frame_new_take(jxlw_color_format_e format, const jxlw_frame_args_t* args, uint8_t* buff) {
    if (!(args && args->width && args->height && buff)) [[unlikely]] return NULL;
    const uint64_t need = (uint64_t)args->width * (uint64_t)args->height * (1 + format);
    if (UINT32_MAX < need || args->size < need) [[unlikely]] return NULL;

    jxlw_frame_t* f = (jxlw_frame_t*)jxlw_malloc(sizeof(jxlw_frame_t));
    if (!f) [[unlikely]] return NULL;
    f->width = args->width;
    f->height = args->height;
    f->size = args->size;
    f->pixel = buff;
    f->format = format;
    std::atomic_init(&f->ref, 1);
    return f;
}

jxlw_frame_t* jxlw_frame_new(jxlw_color_format_e format, const jxlw_frame_args_t* args, const uint8_t* buff) {
    if (!(args && args->width && args->height && buff)) [[unlikely]] return NULL;
    const uint64_t need = (uint64_t)args->width * (uint64_t)args->height * (1 + format);
    if (UINT32_MAX < need || args->size < need) [[unlikely]] return NULL;

    uint8_t* pixel = (uint8_t*)jxlw_malloc(args->size);
    if (!pixel) [[unlikely]] return NULL;
    std::memcpy(pixel, buff, args->size);
    return jxlw_frame_new_take(format, args, pixel);
}

uint32_t jxlw_frame_calc_bytes(jxlw_frame_t* f) {
    return f->width * f->height * (1 + f->format);
}

#pragma endregion // misc_frame
#pragma region misc_vfs
typedef enum _jxlw_file_source_mode {
    E_FILE_SOURCE_READ,
    E_FILE_SOURCE_WRITE,
    E_FILE_SOURCE_CNT
} jxlw_file_source_mode_e;

#ifdef _WIN32
#include <windows.h>

typedef struct _jxlw_file_source {
    jxlw_vfs_source_t core;
    HANDLE hwnd;
} jxlw_file_source_t;

static int32_t jxlw_file_source_read(uint8_t* buff, uint32_t bytes, void* usd) {
    if (!bytes) return 0;
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (buff && src && INVALID_HANDLE_VALUE != src->hwnd) {
        DWORD got = 0;
        if (ReadFile(src->hwnd, buff, (DWORD)bytes, &got, NULL))
            return (int32_t)got;
    }
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_write(const uint8_t* buff, uint32_t bytes, void* usd) {
    if (!bytes) return 0;
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (buff && src && INVALID_HANDLE_VALUE != src->hwnd) {
        DWORD wrote = 0;
        if (WriteFile(src->hwnd, buff, (DWORD)bytes, &wrote, NULL))
            return (int32_t)wrote;
    }
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_seek(int32_t offset, jxlw_file_seek_e whence, void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && INVALID_HANDLE_VALUE != src->hwnd) {
        LARGE_INTEGER res, target = {
            .QuadPart = offset
        };
        if (SetFilePointerEx(src->hwnd, target, &res, whence))
            return (int32_t)(res.QuadPart & ~0u);
    }
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_tell(void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && INVALID_HANDLE_VALUE != src->hwnd) {
        LARGE_INTEGER pos1, pos2 = {};
        if (SetFilePointerEx(src->hwnd, pos2, &pos1, FILE_CURRENT))
            return (int32_t)(pos1.QuadPart & ~0u);
    }
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_query(void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && INVALID_HANDLE_VALUE != src->hwnd) [[likely]] {
        LARGE_INTEGER sz;
        if (GetFileSizeEx((HANDLE)src->hwnd, &sz)) [[likely]]
            return (int32_t)(sz.QuadPart & ~0u);
    }
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static void jxlw_file_source_destroy(jxlw_file_source_t* src) {
    if (src && INVALID_HANDLE_VALUE != src->hwnd) [[likely]] {
        CloseHandle(src->hwnd);
        src->hwnd = INVALID_HANDLE_VALUE;
    }
}

// 关闭文件句柄，结构体本身由调用者管理（它可能是栈上的变量）
static void jxlw_file_source_close(void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    jxlw_file_source_destroy(src);
}

static bool jxlw_file_source_init(jxlw_file_source_t* src, const char* path, jxlw_file_source_mode_e mode) {
    int wl;
    if (src && path) [[likely]] {
        wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
        if (wl > 0) [[likely]] goto step2;
        g_last_status = JXLW_STATUS_ERR_UNDEFINED;
    }
    else
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return false;
step2:

    auto wp = jxlw_malloc_make<wchar_t>(wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp.get(), wl);

    DWORD access = GENERIC_READ;
    DWORD flag = OPEN_EXISTING;
    DWORD share = FILE_SHARE_READ;
    if (E_FILE_SOURCE_READ != mode) {
        access = GENERIC_WRITE;
        flag = CREATE_ALWAYS;
        share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    }
    HANDLE hwnd = CreateFileW(wp.get(), access, share, NULL, flag, 0, NULL);
    if (INVALID_HANDLE_VALUE != hwnd) [[likely]] {
        static const jxlw_vfs_source_t defs = {
            .read = jxlw_file_source_read,
            .write = jxlw_file_source_write,
            .seek = jxlw_file_source_seek,
            .tell = jxlw_file_source_tell,
            .query = jxlw_file_source_query,
            .close = jxlw_file_source_close,
        };
        std::memcpy(src, &defs, sizeof(jxlw_vfs_source_t));
        src->hwnd = hwnd;
        return true;
    }
    g_last_status = JXLW_STATUS_ERR_IO;
    return false;
}

#else   //  _WIN32
#include <fcntl.h>
#include <unistd.h>

typedef struct _jxlw_file_source {
    jxlw_vfs_source_t core;
    int fd;
} jxlw_file_source_t;

static int32_t jxlw_file_source_read(uint8_t* buff, uint32_t bytes, void* usd) {
    if (!bytes) return 0;
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && 0 <= src->fd) [[likely]]
        return (int32_t)read(src->fd, buff, (size_t)bytes);
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_write(const uint8_t* buff, uint32_t bytes, void* usd) {
    if (!bytes) return 0;
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && 0 <= src->fd) [[likely]]
        return (int32_t)write(src->fd, buff, (size_t)bytes);
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_seek(int32_t offset, jxlw_file_seek_e whence, void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && 0 <= src->fd) [[likely]]
        return (int32_t)lseek(src->fd, (off_t)offset, whence);
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_tell(void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && 0 <= src->fd) [[likely]]
        return (int32_t)lseek(src->fd, 0, SEEK_CUR);
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static int32_t jxlw_file_source_query(void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    if (src && 0 <= src->fd) [[likely]] {
        off_t cur = lseek(src->fd, 0, SEEK_CUR);
        if (0 <= cur) [[likely]] {
            off_t end = lseek(src->fd, 0, SEEK_END);
            lseek(src->fd, cur, SEEK_SET);
            return (int32_t)end;
        }
    }
    g_last_status = JXLW_STATUS_ERR_IO;
    return -1;
}

static void jxlw_file_source_destroy(jxlw_file_source_t* src) {
    if (src && 0 <= src->fd) [[likely]]  {
        close(src->fd);
        src->fd = -1;
    }
}

// 关闭文件句柄，结构体本身由调用者管理（它可能是栈上的变量）
static void jxlw_file_source_close(void* usd) {
    jxlw_file_source_t* src = (jxlw_file_source_t*)usd;
    jxlw_file_source_destroy(src);
}

static bool jxlw_file_source_init(jxlw_file_source_t* src, const char* path, jxlw_file_source_mode_e mode) {
    if (src && path) {
        if (E_FILE_SOURCE_READ != mode)
            src->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        else
            src->fd = open(path, O_RDONLY);
        if (0 <= src->fd)
            goto step2;
        g_last_status = JXLW_STATUS_ERR_IO;
    }
    else
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return false;
step2:
    static const jxlw_vfs_source_t defs = {
        .read = jxlw_file_source_read,
        .write = jxlw_file_source_write,
        .seek = jxlw_file_source_seek,
        .tell = jxlw_file_source_tell,
        .query = jxlw_file_source_query,
        .close = jxlw_file_source_close,
    };
    std::memcpy(src, &defs, sizeof(jxlw_vfs_source_t));
    return true;
}

#endif  //  _WIN32
typedef struct _jxlw_mem_source {
    jxlw_vfs_source_t core;
    uint8_t* buff;
    uint32_t size, pos;
} jxlw_mem_source_t;

static int32_t jxlw_mem_source_read(uint8_t* buff, uint32_t bytes, void* usd) {
    if (!bytes) return 0;
    jxlw_mem_source_t* src = (jxlw_mem_source_t*)usd;
    if (buff && src) {
        if (bytes > src->size - src->pos)
            bytes = src->size - src->pos;
        std::memcpy(buff, src->buff + src->pos, bytes);
        src->pos += bytes;
        return (int32_t)bytes;
    }
    return -1;
}

static int32_t jxlw_mem_source_write(const uint8_t* buff, uint32_t bytes, void* usd) {
    if (!bytes) return 0;
    jxlw_mem_source_t* src = (jxlw_mem_source_t*)usd;
    if (buff && src) [[likely]] {
        if (bytes > UINT32_MAX - src->pos) [[unlikely]] {
            g_last_status = JXLW_STATUS_ERR_OUT_OF_RANGE;
            return -1;
        }
        uint32_t need = src->pos + bytes;
        if (need > src->size) {
            need = jxlw_calc_align_size(need);
            uint8_t* grown = (uint8_t*)jxlw_realloc(src->buff, need);
            if (!grown) [[unlikely]] {
                g_last_status = JXLW_STATUS_ERR_OUT_OF_MEMORY;
                return -1;
            }
            src->buff = grown;
            src->size = need;
        }
        std::memcpy(src->buff + src->pos, buff, bytes);
        src->pos += bytes;
        return (int32_t)bytes;
    }
    g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return -1;
}

static int32_t jxlw_mem_source_seek(int32_t offset, jxlw_file_seek_e whence, void* usd) {
    jxlw_mem_source_t* src = (jxlw_mem_source_t*)usd;
    if (src && src->buff) [[likely]] {
        if (JXLW_FILE_SEEK_SET == whence) {
            if (0 <= offset && offset <= (int32_t)src->size) [[likely]] {
                src->pos = (uint32_t)offset;
                return offset;
            }
        }
        else if (JXLW_FILE_SEEK_CUR == whence) {
            int32_t next = (int32_t)src->pos + offset;
            if (0 <= next && next <= (int32_t)src->size) [[likely]] {
                src->pos = (uint32_t)next;
                return next;
            }
        }
        else if (JXLW_FILE_SEEK_END == whence) {
            int32_t next = (int32_t)src->size + offset;
            if (0 <= next && next <= (int32_t)src->size) [[likely]] {
                src->pos = (uint32_t)next;
                return next;
            }
        }
    }
    g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return -1;
}

static int32_t jxlw_mem_source_tell(void* usd) {
    jxlw_mem_source_t* src = (jxlw_mem_source_t*)usd;
    if (src && src->buff) [[likely]]
        return (int32_t)src->pos;
    g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return -1;
}

static int32_t jxlw_mem_source_query(void* usd) {
    jxlw_mem_source_t* src = (jxlw_mem_source_t*)usd;
    if (src && src->buff) [[likely]]
        return (int32_t)src->size;
    g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return -1;
}

static void jxlw_mem_source_destroy(jxlw_mem_source_t* src) {
    if (src && src->buff) [[likely]] {
        jxlw_free(src->buff);
        memset(src, 0, sizeof(jxlw_mem_source_t));
    }
}

static bool jxlw_mem_source_init_take(jxlw_mem_source_t* src, uint8_t* buff, uint32_t size) {
    if (!src) [[unlikely]] {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return false;
    }

    static const jxlw_vfs_source_t defs = {
        .read = jxlw_mem_source_read,
        .write = jxlw_mem_source_write,
        .seek = jxlw_mem_source_seek,
        .tell = jxlw_mem_source_tell,
        .query = jxlw_mem_source_query,
        // .close = jxlw_mem_source_close,
    };
    std::memcpy(src, &defs, sizeof(jxlw_vfs_source_t));
    src->buff = buff;
    src->size = size;
    src->pos = 0;
    return true;
}

static bool jxlw_mem_source_init(jxlw_mem_source_t* src, const uint8_t* buff, uint32_t size) {
    if (buff && size) {
        uint8_t* copy = (uint8_t*)jxlw_malloc(size);
        std::memcpy(copy, buff, size);
        return jxlw_mem_source_init_take(src, copy, size);
    }
    return jxlw_mem_source_init_take(src, NULL, 0);
}

struct jxlw_vfs_source_guard {
    const jxlw_vfs_source_t* src = nullptr;
    void* usd = nullptr;
    bool succ = false;
    ~jxlw_vfs_source_guard() {
        if (succ && src && src->close) src->close(usd);
    }
    jxlw_vfs_source_guard(const jxlw_vfs_source_t* src, void* usd) : src(src), usd(usd) {
        succ = !src->open || src->open(usd);
    }
    explicit operator bool() const { return succ; }
};

#pragma endregion // misc_vfs
#pragma endregion // misc
#pragma region // reader

typedef struct _jxlw_reader {
    jxlw_reader_backend_t* be;
    // jxlw_vfs_source_t src;  // 源的拷贝，销毁的时候用它来关闭源
    // void* usd;              // 源的用户数据
    jxlw_codec_type_e type;
    std::atomic_uint_fast32_t ref;
} jxlw_reader_t;

/**
 * Read the whole stream into a new buffer.
 *
 * The buffer belongs to the caller, it has to be released with jxlw_free().
 */

static uint8_t* jxlw_reader_read_all(const jxlw_vfs_source_t* src, void* usd, uint32_t* size) {
    if (!src->read) [[unlikely]] {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }
    if (jxlw_vfs_source_guard guard(src, usd); guard) [[likely]] {
        if (src->seek) {
            // 可以 seek：先算出大小，然后一次读完
            int32_t len = src->seek(0, JXLW_FILE_SEEK_END, usd);
            if (0 >= len) [[unlikely]] {
                g_last_status = JXLW_STATUS_ERR_IO;
                return NULL;
            }
            uint8_t* buff = (uint8_t*)jxlw_malloc((size_t)len);
            if (!buff) [[unlikely]] {
                g_last_status = JXLW_STATUS_ERR_OUT_OF_MEMORY;
                return NULL;
            }
            if (0 > src->seek(0, JXLW_FILE_SEEK_SET, usd)) [[unlikely]] {
                jxlw_free(buff);
                g_last_status = JXLW_STATUS_ERR_IO;
                return NULL;
            }
            int32_t got = src->read(buff, (uint32_t)len, usd);
            if (0 < got) {
                *size = (uint32_t)got;
                return buff;
            }
            jxlw_free(buff);
            g_last_status = JXLW_STATUS_ERR_IO;
            return NULL;
        }
        // 不能 seek：一段一段地读
        uint8_t* chunk = static_cast<uint8_t*>(jxlw_malloc(DEF_BUFF_SIZE));
        c_byte_array_t* arr = c_byte_array_sized_new(DEF_BUFF_SIZE);
        while (UINT32_MAX > arr->len) {
            int32_t got = src->read(chunk, DEF_BUFF_SIZE, usd);
            if (0 >= got) break;
            c_byte_array_append(arr, chunk, (uint32_t)got);
        }
        jxlw_free(chunk);
        if (arr->len) [[likely]] {
            *size = arr->len;
            return c_byte_array_free(arr, false);
        }
        c_byte_array_free(arr, true);
    }
    g_last_status = JXLW_STATUS_ERR_OPERATION_FAILED;
    return NULL;
}

JXLW_API void jxlw_reader_unref(jxlw_reader_t* rd) {
    if (!rd || 1 < std::atomic_fetch_sub_explicit(&rd->ref, 1, std::memory_order_release)) return;
    rd->be->notify(rd->be);
    // if (rd->src.close) rd->src.close(rd->usd);
    jxlw_free(rd);
}

JXLW_API jxlw_reader_t* jxlw_reader_ref(jxlw_reader_t* rd) {
    std::atomic_fetch_add_explicit(&rd->ref, 1, std::memory_order_relaxed);
    return rd;
}

JXLW_API jxlw_reader_t* jxlw_reader_new(const jxlw_vfs_source_t* src, void* usd) {
    if (!src || !src->read) [[unlikely]] {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }

    uint32_t size = 0;
    auto buff = std::unique_ptr<uint8_t, decltype(&jxlw_free)>(jxlw_reader_read_all(src, usd, &size), jxlw_free);
    if (!buff || !size) {
        g_last_status = JXLW_STATUS_ERR_IO;
        return NULL;
    }
    jxlw_codec_type_e type = JXLW_CODEC_TYPE_JXL;
    jxlw_reader_backend_t* be = jxlw_reader_jxl_backend_new(buff.get(), size);
    if (!be) {
        type = JXLW_CODEC_TYPE_JPEG;
        be = jxlw_reader_jpeg_backend_new(buff.get(), size);
        if (!be) {
            g_last_status = JXLW_STATUS_ERR_UNKNOW_FORMAT;
            return NULL;
        }
    }

    jxlw_reader_t* rd = static_cast<jxlw_reader_t*>(jxlw_malloc(sizeof(jxlw_reader_t)));
    std::atomic_init(&rd->ref, 1);
    rd->type = type;
    rd->be = be;
    return rd;
}

JXLW_API jxlw_reader_t* jxlw_reader_new_from_file(const char* path) {
    if (!path) [[unlikely]] {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }
    jxlw_file_source_t src = {}; //std::unique_ptr<jxlw_file_source_t, decltype(&jxlw_free)>(jxlw_malloc(sizeof(jxlw_file_source_t)));
    if (jxlw_file_source_init(&src, path, E_FILE_SOURCE_READ)) {
        jxlw_reader_t* rd = jxlw_reader_new(&src.core, &src);
        jxlw_file_source_destroy(&src);
        return rd;
    }

    return NULL;
}

JXLW_API jxlw_reader_t* jxlw_reader_new_from_memory(const uint8_t* buff, uint32_t size) {
    if (!buff || !size) [[unlikely]] {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }

    jxlw_mem_source_t src = {};
    if (jxlw_mem_source_init(&src, buff, size)) {
        jxlw_reader_t* rd = jxlw_reader_new(&src.core, &src);
        jxlw_mem_source_destroy(&src);
        return rd;
    }
    return NULL;
}

JXLW_API uint8_t* jxlw_reader_read_frame(jxlw_reader_t* rd, uint32_t idx, jxlw_frame_args_t* args) {
    if (!(rd && rd->be)) [[unlikely]] {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }
    // 帧在创建 reader 的时候就已经解码好了，这里只是取出来
    jxlw_frame_t* f = rd->be->read(rd->be, idx, &g_last_status);
    if (!f) return NULL;

    uint8_t* pixel = NULL;
    if (f->pixel && f->size) [[likely]] {
        if (args) {
            args->width = f->width;
            args->height = f->height;
            args->size = f->size;
        }
        pixel = (uint8_t*)jxlw_malloc(f->size);
        if (pixel) std::memcpy(pixel, f->pixel, f->size);
        else g_last_status = JXLW_STATUS_ERR_OUT_OF_MEMORY;
    }
    else
        g_last_status = JXLW_STATUS_ERR_UNDEFINED;
    jxlw_frame_unref(f);
    return pixel;
}

JXLW_API bool jxlw_reader_get_args(jxlw_reader_t* rd, jxlw_reader_args_t* args) {
    if (!(rd && rd->be && args)) [[unlikely]] {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return false;
    }
    // 尺寸和格式在创建 reader 的时候解码出来了
    args->format = rd->be->format;
    args->type = rd->type;
    args->width = rd->be->width;
    args->height = rd->be->height;
    args->frames = rd->be->frames;
    args->duration = rd->be->duration;
    return true;
}
#pragma endregion // reader
#pragma region writer


typedef struct _jxlw_writer {
    jxlw_backend_writer_t* be;
    uint8_t* dst;
    jxlw_frame_args_t args;
    std::atomic_uint_fast32_t ref;
} jxlw_writer_t;

// 把编码好的数据写进 src
static void jxlw_writer_write(jxlw_writer_t* wr, jxlw_vfs_source_t* src, void* usd) {
    if (wr && wr->dst && src && src->write) {
        if (src->open && !src->open(usd)) {
            g_last_status = JXLW_STATUS_ERR_OPERATION_FAILED;
            return;
        }

        // 注意：用户数据是 usd，不是 src 自己
        int wrote = src->write(wr->dst, wr->args.size, usd);
        if (wrote != (int)wr->args.size)
            g_last_status = JXLW_STATUS_ERR_IO;
        else
            g_last_status = JXLW_STATUS_OK;

        if (src->close) src->close(usd);
        return;
    }
    g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
}


JXLW_API jxlw_writer_t* jxlw_writer_new(const jxlw_writer_args_t* args) {
    if (!args) {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }
    jxlw_writer_t* wr = static_cast<jxlw_writer_t*>(jxlw_malloc(sizeof(jxlw_writer_t)));
    if (!wr) {
        g_last_status = JXLW_STATUS_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    std::memset((void*)wr, 0, sizeof(jxlw_writer_t));
    if (JXLW_CODEC_TYPE_JPEG != args->type)
        wr->be = jxlw_writer_jxl_backend_new(args);
    else
        wr->be = jxlw_writer_jpeg_backend_new(args);
    if (wr->be) {
        std::atomic_init(&wr->ref, 1);
        return wr;
    }
    jxlw_free(wr);
    return NULL;
}

// Increment the reference counter of the writer.
JXLW_API jxlw_writer_t* jxlw_writer_ref(jxlw_writer_t* wr) {
    std::atomic_fetch_add_explicit(&wr->ref, 1, std::memory_order_relaxed);
    return wr;
}
//
// call this multi time will output jxl
JXLW_API jxlw_status_e jxlw_writer_add_frame(jxlw_writer_t* wr, jxlw_frame_args_t* args, const uint8_t* buff) {
    if (wr && !wr->dst && args && buff)
        g_last_status = wr->be->write(wr->be, args, buff);
    else
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return g_last_status;
}

/**
 * Encode the collected frames, write them to `src` and release the writer.
 *
 * The writer is always destroyed, even when the encoding fails. The `close`
 * callback of `src` is called at the end, the writer takes the ownership of
 * the source.
 */
JXLW_API jxlw_status_e jxlw_writer_unref(jxlw_writer_t* wr, jxlw_vfs_source_t* src, void* usd) {
    if (!wr) {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return g_last_status;
    }
    if (src && src->write) {
        if (!wr->dst) {
            // 只有第一次调用才真的编码，后面的调用复用结果
            wr->dst = wr->be->finish(wr->be, &wr->args);
            if (!wr->dst) {
                g_last_status = JXLW_STATUS_ERR_ENCODE_FAILED;
                goto done;
            }
        }
        jxlw_writer_write(wr, src, usd);
    }
    else {
        // 没有目标的时候只释放 writer，不做编码
        g_last_status = JXLW_STATUS_OK;
    }
done:
    if (1 >= std::atomic_fetch_sub_explicit(&wr->ref, 1u, std::memory_order_release)) {
        jxlw_free(wr->dst);
        // 后端也要释放，编码器和线程池都在里面
        if (wr->be) wr->be->notify(wr->be);
        wr->be = NULL;
        jxlw_free(wr);
    }
    return g_last_status;
}

// Same as jxlw_writer_unref() with a file as the destination.
JXLW_API jxlw_status_e jxlw_writer_unref_to_file(jxlw_writer_t* wr, const char* path) {
    if (wr && path) {
        jxlw_file_source_t src = {};
        if (jxlw_file_source_init(&src, path, E_FILE_SOURCE_WRITE)) {
            jxlw_status_e st = jxlw_writer_unref(wr, &src.core, &src);
            jxlw_file_source_destroy(&src);
            return st;
        }
        g_last_status = JXLW_STATUS_ERR_IO;
    }
    else
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
    return g_last_status;
}

JXLW_API uint8_t* jxlw_writer_unref_to_bytes(jxlw_writer_t* wr, uint32_t* size) {
    if (size) *size = 0;
    if (!wr) {
        g_last_status = JXLW_STATUS_ERR_BAD_ARGUMENT;
        return NULL;
    }

    jxlw_mem_source_t src = {};
    if (!jxlw_mem_source_init(&src, NULL, 0)) {
        g_last_status = JXLW_STATUS_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    jxlw_status_e st = jxlw_writer_unref(wr, &src.core, &src);
    // 缓冲区的所有权交给调用方，这里不能释放
    uint8_t* buff = src.buff;
    uint32_t len = src.size;
    src.buff = NULL;
    src.size = 0;
    jxlw_mem_source_destroy(&src);

    if (JXLW_STATUS_OK != st || !buff || !len) {
        jxlw_free(buff);
        if (JXLW_STATUS_OK == st) g_last_status = JXLW_STATUS_ERR_ENCODE_FAILED;
        return NULL;
    }
    if (size) *size = len;
    return buff;
}
#pragma endregion // writer
