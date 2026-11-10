#include <assert.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cter.h"

#pragma region misc
#pragma region diagnostics

#if defined(__clang__) || defined(__GNUC__)
#define C_LIKELY(expr) __builtin_expect(!!(expr), 1)
#define C_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#else
#define C_LIKELY(expr) (expr)
#define C_UNLIKELY(expr) (expr)
#endif
#ifdef CTER_DEBUG
static void cter_log_warning(const char* func, const char* expr)
{
    fprintf(stderr, "%s: %s\n", func, expr);
}

#define cter_return_if_fail(expr)         \
    do {                                  \
        if (C_UNLIKELY(!(expr))) {\
            cter_log_warning(__func__, #expr); \
            return;                       \
        }                                 \
    } while (0)

#define cter_return_if_true(expr)         \
    do {                                  \
        if (C_UNLIKELY(expr)) {\
            cter_log_warning(__func__, #expr); \
            return;                       \
        }                                 \
    } while (0)

#define cter_return_val_if_fail(expr, val) \
    do {                                   \
        if (C_UNLIKELY(!(expr))) {\
            cter_log_warning(__func__, #expr); \
            return (val);                  \
        }                                  \
    } while (0)

#define cter_return_val_if_true(expr, val) \
    do {                                   \
        if (C_UNLIKELY(expr)) {\
            cter_log_warning(__func__, #expr); \
            return (val);                  \
        }                                  \
    } while (0)
#else
#define cter_return_if_fail(expr) // do { (void)sizeof(expr); } while (0)
#define cter_return_if_true(expr) // do { (void)sizeof(expr); } while (0)
#define cter_return_val_if_fail(expr, val) // do { (void)sizeof(expr); } while (0)
#define cter_return_val_if_true(expr, val) // do { (void)sizeof(expr); } while (0)
#endif

#pragma endregion
#pragma region alloc

typedef enum _cter_alloc_state : uint_fast32_t {
    CTER_ALLOC_STATE_NONE = 0,
    CTER_ALLOC_STATE_SETTING = 1,
    CTER_ALLOC_STATE_INITED = 2
} cter_alloc_state_t;

static void* cter_malloc(size_t size);
static void* cter_realloc(void* ptr, size_t size);
static atomic_uint_fast32_t def_alloc_state = CTER_ALLOC_STATE_NONE;
static c_allocator_t def_alloc = {
    cter_malloc,
    cter_realloc,
    free
};

#define cter_malloc0(size) memset(cter_malloc(size), 0, size)
#define cter_new(type) ((type*)cter_malloc(sizeof(type)))
#define cter_newn(type, n) ((type*)cter_malloc(sizeof(type) * (n)))
#define cter_new0(type) ((type*)cter_malloc0(sizeof(type)))
#define cter_new0n(type, n) ((type*)cter_malloc0(sizeof(type) * (n)))
#define cter_clean_pointer(ptr) do { if (ptr) { def_alloc.free(ptr); ptr = NULL; } } while (0)

static void cter_set_default_allocator(void)
{
    cter_alloc_state_t exp = CTER_ALLOC_STATE_NONE;
    if (atomic_compare_exchange_strong_explicit(&def_alloc_state, &exp, CTER_ALLOC_STATE_SETTING, memory_order_release, memory_order_acquire)) {
        def_alloc.malloc = malloc;
        def_alloc.realloc = realloc;
        def_alloc.free = free;
        atomic_store_explicit(&def_alloc_state, CTER_ALLOC_STATE_INITED, memory_order_release);
        return;
    }

    while (CTER_ALLOC_STATE_INITED > atomic_load_explicit(&def_alloc_state, memory_order_acquire));
}

static void* cter_malloc(size_t size)
{
    cter_return_val_if_fail(size, NULL);
    if (C_UNLIKELY(!atomic_load_explicit(&def_alloc_state, memory_order_acquire)))
        cter_set_default_allocator();
    return def_alloc.malloc(size);
}

static void* cter_realloc(void* ptr, size_t size)
{
    cter_return_val_if_fail(size, NULL);
    if (!ptr) return cter_malloc(size);
    if (C_UNLIKELY(!atomic_load_explicit(&def_alloc_state, memory_order_acquire)))
        cter_set_default_allocator();
    return def_alloc.realloc(ptr, size);
}

static void* cter_memdup(const void* data, size_t size)
{
    cter_return_val_if_fail(data && size, NULL);
    return memcpy(cter_malloc(size), data, size);
}

static void cter_free(void* ptr) {
    def_alloc.free(ptr);
}

static void cter_free_wrap(void* data, void* usd)
{
    (void)usd;
    cter_free(data);
}

static void* cter_steal_pointer_impl(void** value)
{
    void* res = *value;
    *value = NULL;
    return res;
}

#define cter_steal_pointer(value_ptr) cter_steal_pointer_impl((void**)(value_ptr))

bool c_set_allocator(const c_allocator_t* alloc) {
    cter_return_val_if_fail(alloc, false);
    cter_return_val_if_fail(alloc->malloc && alloc->realloc && alloc->free, false);
    cter_alloc_state_t exp = CTER_ALLOC_STATE_NONE;
    if (atomic_compare_exchange_strong_explicit(&def_alloc_state, &exp, CTER_ALLOC_STATE_SETTING, memory_order_release, memory_order_acquire)) {
        memcpy(&def_alloc, alloc, sizeof(c_allocator_t));
        atomic_store_explicit(&def_alloc_state, CTER_ALLOC_STATE_INITED, memory_order_release);
        return true;
    }
    
    return false;
}

#pragma endregion // alloc

static bool cter_add_size(size_t a, size_t b, size_t* out)
{
    if (a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static bool cter_mul_size(size_t a, size_t b, size_t* out)
{
    if (a != 0 && b > SIZE_MAX / a)
        return false;
    *out = a * b;
    return true;
}

static bool cter_add_u32(uint32_t a, uint32_t b, uint32_t* out)
{
    if (a > UINT32_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static size_t cter_nearest_pow2(size_t value)
{
    size_t n = value ? value - 1u : 0u;
    for (size_t shift = 1; shift < sizeof(size_t) * 8u; shift <<= 1u)
        n |= n >> shift;
    return n + 1u;
}

static void cter_zero_bytes(void* data, size_t size)
{
    if (data && size)
        memset(data, 0, size);
}

typedef struct _cter_sort_ctx {
    char* base;
    char* tmp;
    size_t element_size;
    pfn_cter_compare_data compare_func;
    void* usd;
} cter_sort_ctx_t;

static void cter_merge_sort_range(cter_sort_ctx_t* ctx, size_t left, size_t right)
{
    if (C_UNLIKELY(right - left < 2u))
        return;

    const size_t mid = left + (right - left) / 2u;
    cter_merge_sort_range(ctx, left, mid);
    cter_merge_sort_range(ctx, mid, right);

    size_t i = left;
    size_t j = mid;
    size_t k = left;

    while (i < mid && j < right) {
        char* lhs = ctx->base + i * ctx->element_size;
        char* rhs = ctx->base + j * ctx->element_size;

        if (ctx->compare_func(lhs, rhs, ctx->usd) <= 0) {
            memcpy(ctx->tmp + k * ctx->element_size, lhs, ctx->element_size);
            i++;
        }
        else {
            memcpy(ctx->tmp + k * ctx->element_size, rhs, ctx->element_size);
            j++;
        }

        k++;
    }

    while (i < mid) {
        memcpy(ctx->tmp + k * ctx->element_size, ctx->base + i * ctx->element_size, ctx->element_size);
        i++;
        k++;
    }

    while (j < right) {
        memcpy(ctx->tmp + k * ctx->element_size, ctx->base + j * ctx->element_size, ctx->element_size);
        j++;
        k++;
    }

    memcpy(ctx->base + left * ctx->element_size, ctx->tmp + left * ctx->element_size, (right - left) * ctx->element_size);
}

static void cter_sort(void* base, size_t count, size_t element_size, const pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_true(!base || count < 2u || !element_size || !compare_func);
    size_t alloc_size = 0u;
    cter_mul_size(count, element_size, &alloc_size);

    char* tmp = cter_malloc(alloc_size);
    cter_sort_ctx_t ctx = {
        .base = (char*)base,
        .tmp = tmp,
        .element_size = element_size,
        .compare_func = compare_func,
        .usd = usd,
    };

    cter_merge_sort_range(&ctx, 0u, count);
    cter_free(tmp);
}

static int cter_compare_adapter(const void* a, const void* b, void* usd)
{
    return (*(const pfn_cter_compare*)usd)(a, b);
}

bool c_direct_equal(const void* a, const void* b)
{
    return a == b;
}

bool c_str_equal(const void* a, const void* b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    return strcmp((const char*)a, (const char*)b) == 0;
}

#pragma endregion // misc
#pragma region containers
#pragma region array

static bool cter_array_expand(c_array_t* array, uint32_t extra)
{
    uint32_t needed = 0u;
    cter_return_val_if_fail(array && array->element_size > 0u, false);
    if (!cter_add_u32(array->len, extra, &needed))
        return false;
    if (needed > UINT32_MAX - 1u)
        return false;
    needed += 1u;

    if (needed <= array->capacity)
        return true;

    size_t bytes = 0u;
    size_t grown = cter_nearest_pow2((size_t)needed * (size_t)array->element_size);
    if (grown < 16u)
        grown = 16u;
    bytes = grown;

    const uint32_t old_capacity = array->capacity;
    uint8_t* data = cter_realloc(array->data, bytes);
    if (!data)
        return false;
    array->data = data;
    array->capacity = (uint32_t)(bytes / array->element_size);
    if (array->capacity < needed)
        array->capacity = needed;

    if (array->capacity > old_capacity) {
        size_t zero_offset = (size_t)old_capacity * (size_t)array->element_size;
        size_t zero_size = ((size_t)array->capacity - (size_t)old_capacity) * (size_t)array->element_size;
        cter_zero_bytes(array->data + zero_offset, zero_size);
    }
    return true;
}

uint8_t* c_array_free(c_array_t* arr, bool free_segment)
{
    cter_return_val_if_fail(arr, NULL);

    uint8_t* res = NULL;
    if (free_segment) 
        cter_free(arr->data);
    else
        res = cter_steal_pointer((void**)&arr->data);
    cter_free(arr);
    return res;
}

void c_array_unref(c_array_t* array)
{
    if (!array || 1u < atomic_fetch_sub_explicit(&array->ref, 1u, memory_order_acq_rel))
        return;
    c_array_free(array, true);
}

c_array_t* c_array_ref(c_array_t* array)
{
    cter_return_val_if_fail(array, NULL);
    atomic_fetch_add_explicit(&array->ref, 1u, memory_order_relaxed);
    return array;
}

c_array_t* c_array_new_full(uint32_t reserved_size, uint32_t element_size)
{
    cter_return_val_if_fail(element_size > 0u, NULL);
    c_array_t* arr = cter_malloc0(sizeof(c_array_t));
    atomic_init(&arr->ref, 1u);
    arr->element_size = element_size;

    if (reserved_size)
        cter_array_expand(arr, reserved_size);

    return arr;
}

c_array_t* c_array_new_take(void* data, uint32_t len, uint32_t element_size)
{
    cter_return_val_if_fail(element_size > 0u, NULL);
    cter_return_val_if_fail(data || !len, NULL);

    c_array_t* arr = c_array_new_full(0u, element_size);
    arr->data = data;
    arr->len = len;
    arr->capacity = len;
    return arr;
}

c_array_t* c_array_new(uint32_t element_size)
{
    return c_array_new_full(0u, element_size);
}

void* c_array_steal(c_array_t* array, uint32_t* len)
{
    cter_return_val_if_fail(array, NULL);
    if (len)
        *len = array->len;
    void* data = cter_steal_pointer((void**)&array->data);
    array->len = 0u;
    array->capacity = 0u;
    return data;
}

c_array_t* c_array_copy(c_array_t* array)
{
    cter_return_val_if_fail(array, NULL);
    c_array_t* copy = c_array_new_full(array->len, array->element_size);
    cter_return_val_if_fail(copy, NULL);
    copy->len = array->len;
    if (array->data && array->len)
        memcpy(copy->data, array->data, (size_t)array->len * (size_t)array->element_size);
    return copy;
}

c_array_t* c_array_append_vals(c_array_t* array, const void* data, uint32_t len)
{
    cter_return_val_if_fail(array, NULL);
    if (!len)
        return array;
    cter_return_val_if_fail(data, NULL);

    if (C_UNLIKELY(!cter_array_expand(array, len)))
        return NULL;
    memcpy(array->data + (size_t)array->len * (size_t)array->element_size, data, (size_t)len * (size_t)array->element_size);
    array->len += len;
    return array;
}

c_array_t* c_array_prepend_vals(c_array_t* array, const void* data, uint32_t len)
{
    cter_return_val_if_fail(array, NULL);
    if (C_UNLIKELY(!len))
        return array;
    cter_return_val_if_fail(data, NULL);

    if (C_UNLIKELY(!cter_array_expand(array, len)))
        return NULL;
    memmove(array->data + (size_t)len * (size_t)array->element_size, array->data, (size_t)array->len * (size_t)array->element_size);
    memcpy(array->data, data, (size_t)len * (size_t)array->element_size);
    array->len += len;
    return array;
}

c_array_t* c_array_insert_vals(c_array_t* array, uint32_t index, const void* data, uint32_t len)
{
    cter_return_val_if_fail(array, NULL);
    cter_return_val_if_fail(index <= array->len, NULL);
    if (C_UNLIKELY(!len))
        return array;
    cter_return_val_if_fail(data, NULL);

    if (index >= array->len)
        return c_array_append_vals(array, data, len);

    if (C_UNLIKELY(!cter_array_expand(array, len)))
        return NULL;
    memmove(array->data + (size_t)(index + len) * (size_t)array->element_size, array->data + (size_t)index * (size_t)array->element_size, (size_t)(array->len - index) * (size_t)array->element_size);
    memcpy(array->data + (size_t)index * (size_t)array->element_size, data, (size_t)len * (size_t)array->element_size);
    array->len += len;
    return array;
}

c_array_t* c_array_remove_index(c_array_t* array, uint32_t index)
{
    cter_return_val_if_fail(array && index < array->len, NULL);
    if (index + 1u < array->len)
        memmove(array->data + (size_t)index * (size_t)array->element_size, array->data + (size_t)(index + 1u) * (size_t)array->element_size, (size_t)(array->len - index - 1u) * (size_t)array->element_size);
    array->len -= 1u;
    cter_zero_bytes(array->data + (size_t)array->len * (size_t)array->element_size, array->element_size);
    return array;
}

c_array_t* c_array_remove_range(c_array_t* array, uint32_t index, uint32_t length)
{
    cter_return_val_if_fail(array && index <= array->len, NULL);
    cter_return_val_if_fail(length <= array->len - index, NULL);
    if (C_UNLIKELY(!length))
        return array;

    if (index + length < array->len) 
        memmove(array->data + (size_t)index * (size_t)array->element_size, array->data + (size_t)(index + length) * (size_t)array->element_size, (size_t)(array->len - index - length) * (size_t)array->element_size);
    array->len -= length;
    cter_zero_bytes(array->data + (size_t)array->len * (size_t)array->element_size, (size_t)length * (size_t)array->element_size);
    return array;
}

void c_array_remove_all(c_array_t* array)
{
    cter_return_if_fail(array);
    cter_zero_bytes(array->data, (size_t)array->capacity * (size_t)array->element_size);
    array->len = 0u;
}

c_array_t* c_array_set_size(c_array_t* array, uint32_t length)
{
    cter_return_val_if_fail(array, NULL);
    if (length > array->len) {
        uint32_t growth = length - array->len;
        if (C_UNLIKELY(!cter_array_expand(array, growth)))
            return NULL;
        cter_zero_bytes(array->data + (size_t)array->len * (size_t)array->element_size, (size_t)growth * (size_t)array->element_size);
    }
    array->len = length;
    return array;
}

void c_array_sort(c_array_t* array, const pfn_cter_compare compare_func)
{
    cter_return_if_fail(array && compare_func);
    if (C_UNLIKELY(!array->data || 2u > array->len)) return;
    cter_sort(array->data, array->len, array->element_size, cter_compare_adapter, (void*)&compare_func);
}

void c_array_sort_with_data(c_array_t* array, const pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(array && compare_func);
    if (C_UNLIKELY(!array->data || 2u > array->len)) return;
    cter_sort(array->data, array->len, array->element_size, compare_func, usd);
}

bool c_array_binary_search(c_array_t* array, const void* target, const pfn_cter_compare compare_func, uint32_t* out_match_index)
{
    cter_return_val_if_fail(array && target && compare_func, false);
    if (C_UNLIKELY(!array->data || 2u > array->len)) return false;

    uint32_t left = 0u;
    uint32_t right = array->len;
    while (left < right) {
        uint32_t mid = left + (right - left) / 2u;
        const void* element = array->data + (size_t)mid * (size_t)array->element_size;
        int cmp = compare_func(element, target);
        if (cmp < 0) {
            left = mid + 1u;
        } else if (cmp > 0) {
            right = mid;
        } else {
            if (out_match_index)
                *out_match_index = mid;
            return true;
        }
    }
    return false;
}
#pragma endregion // array
#pragma region // byte_array

static bool cter_byte_array_expand(c_byte_array_t* array, uint32_t extra)
{
    cter_return_val_if_fail(array, false);
    uint32_t needed = 0u;
    if (C_UNLIKELY(!cter_add_u32(array->len, extra, &needed)))
        return false;
    if (needed > UINT32_MAX - 1u)
        return false;
    needed += 1u;

    if (needed <= array->capacity)
        return true;

    size_t bytes = cter_nearest_pow2((size_t)needed);
    if (bytes < 16u)
        bytes = 16u;

    const uint32_t old_capacity = array->capacity;
    uint8_t* data = cter_realloc(array->data, bytes);
    array->data = data;
    array->capacity = (uint32_t)bytes;
    if (array->capacity > old_capacity)
        cter_zero_bytes(array->data + old_capacity, array->capacity - old_capacity);
    return true;
}

uint8_t* c_byte_array_free(c_byte_array_t* arr, bool free_segment)
{
    cter_return_val_if_fail(arr, NULL);
    uint8_t* res = NULL;
    if (free_segment)
        cter_free(arr->data);
    else
        res = cter_steal_pointer((void**)&arr->data);
    cter_free(arr);
    return res;
}

c_bytes_t* c_byte_array_free_to_bytes(c_byte_array_t* array)
{
    cter_return_val_if_fail(array, NULL);
    size_t size = array->len;
    void* data = c_byte_array_free(array, false);
    return c_bytes_new_take(&data, size);
}

void c_byte_array_unref(c_byte_array_t* arr)
{
    if (!arr || 1 < atomic_fetch_sub_explicit(&arr->ref, 1u, memory_order_acq_rel))
        return;
    c_byte_array_free(arr, true);
}


c_byte_array_t* c_byte_array_ref(c_byte_array_t* array)
{
    cter_return_val_if_fail(array, NULL);
    atomic_fetch_add_explicit(&array->ref, 1u, memory_order_relaxed);
    return array;
}

c_byte_array_t* c_byte_array_new(void)
{
    c_byte_array_t* arr = cter_malloc0(sizeof(c_byte_array_t));
    atomic_init(&arr->ref, 1u);
    return arr;
}

c_byte_array_t* c_byte_array_sized_new(uint32_t reserved_size)
{
    c_byte_array_t* arr = c_byte_array_new();
    cter_return_val_if_fail(arr, NULL);
    if (reserved_size)
        cter_byte_array_expand(arr, reserved_size);
    return arr;
}

c_byte_array_t* c_byte_array_new_take(uint8_t** data, uint32_t len)
{
    cter_return_val_if_fail(data || len == 0u, NULL);
    cter_return_val_if_fail((data && *data) || len == 0u, NULL);
    c_byte_array_t* array = c_byte_array_new();
    cter_return_val_if_fail(array, NULL);
    if (data)
        array->data = cter_steal_pointer((void**)data);
    array->len = len;
    array->capacity = len;
    return array;
}

c_byte_array_t* c_byte_array_append(c_byte_array_t* arr, const uint8_t* data, uint32_t len)
{
    cter_return_val_if_fail(arr && data, NULL);
    if (C_UNLIKELY(!len)) return arr;
    if (!cter_byte_array_expand(arr, len))
        return NULL;
    memcpy(arr->data + arr->len, data, len);
    arr->len += len;
    return arr;
}

c_byte_array_t* c_byte_array_prepend(c_byte_array_t* array, const uint8_t* data, uint32_t len)
{
    cter_return_val_if_fail(array, NULL);
    if (!len)
        return array;
    cter_return_val_if_fail(data, NULL);
    if (!cter_byte_array_expand(array, len))
        return NULL;
    memmove(array->data + len, array->data, array->len);
    memcpy(array->data, data, len);
    array->len += len;
    return array;
}

c_byte_array_t* c_byte_array_remove_index(c_byte_array_t* array, uint32_t index)
{
    cter_return_val_if_fail(array && index < array->len, NULL);
    if (index + 1u < array->len)
        memmove(array->data + index, array->data + index + 1u, array->len - index - 1u);
    array->len -= 1u;
    array->data[array->len] = 0u;
    return array;
}

c_byte_array_t* c_byte_array_remove_range(c_byte_array_t* array, uint32_t index, uint32_t length)
{
    cter_return_val_if_fail(array && index <= array->len, NULL);
    cter_return_val_if_fail(length <= array->len - index, NULL);
    if (!length)
        return array;
    if (index + length < array->len)
        memmove(array->data + index, array->data + index + length, array->len - index - length);
    array->len -= length;
    cter_zero_bytes(array->data + array->len, length);
    return array;
}

c_byte_array_t* c_byte_array_set_size(c_byte_array_t* array, uint32_t length)
{
    cter_return_val_if_fail(array, NULL);
    if (length > array->len) {
        if (!cter_byte_array_expand(array, length - array->len))
            return NULL;
        cter_zero_bytes(array->data + array->len, length - array->len);
    }
    array->len = length;
    return array;
}

uint8_t* c_byte_array_steal(c_byte_array_t* array, uint32_t* len)
{
    cter_return_val_if_fail(array, NULL);
    if (len)
        *len = array->len;
    uint8_t* data = cter_steal_pointer((void**)&array->data);
    array->len = 0u;
    array->capacity = 0u;
    return data;
}

void c_byte_array_sort(c_byte_array_t* array, pfn_cter_compare compare_func)
{
    cter_return_if_fail(array && compare_func);
    pfn_cter_compare fn = compare_func;
    cter_sort(array->data, array->len, sizeof(uint8_t), cter_compare_adapter, &fn);
}

void c_byte_array_sort_with_data(c_byte_array_t* array, pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(array && compare_func);
    cter_sort(array->data, array->len, sizeof(uint8_t), compare_func, usd);
}

c_bytes_t* c_byte_array_to_bytes(c_byte_array_t* array)
{
    cter_return_val_if_fail(array, NULL);
    void* data = cter_memdup(array->data, array->len);
    return c_bytes_new_take(&data, array->len);
}

#pragma endregion // byte_array
#pragma region // bytes

typedef struct _c_bytes {
    void* data;
    size_t size;
    pfn_cter_func notify;
    void* usd;
    atomic_uint_fast32_t ref;
} c_bytes_t;

static void c_bytes_unref_wrap(void*, void* usd) { c_bytes_unref((c_bytes_t*)usd); }

void c_bytes_free(c_bytes_t* bytes) {
    if (C_UNLIKELY(!bytes)) return;
    if (bytes->notify)
        bytes->notify(bytes->data, bytes->usd);
    cter_free(bytes);
}

void* c_bytes_free_to_data(c_bytes_t* bytes, size_t* size) {
    if (C_UNLIKELY(!bytes)) return NULL;

    if (size) *size = bytes->size;

    if (bytes->notify == cter_free_wrap && bytes->usd == bytes->data) {
        void* data = (void*)bytes->data;
        bytes->notify = NULL;
        c_bytes_free(bytes);
        return data;
    }

    void* data = cter_memdup(bytes->data, bytes->size);
    c_bytes_free(bytes);
    return data;
}

void c_bytes_unref(c_bytes_t* bytes)
{
    if (!bytes || 1u < atomic_fetch_sub_explicit(&bytes->ref, 1u, memory_order_acq_rel))
        return;
    c_bytes_free(bytes);
}

static bool cter_bytes_can_steal(const c_bytes_t* bytes)
{
    return bytes->notify == cter_free_wrap && bytes->usd == bytes->data;
}

void* c_bytes_unref_to_data(c_bytes_t* bytes, size_t* size)
{
    if (!bytes)
        return NULL;

    uint_fast32_t expected = 1u;
    if (cter_bytes_can_steal(bytes) && atomic_compare_exchange_strong_explicit(
            &bytes->ref, &expected, 0u, memory_order_acq_rel, memory_order_acquire))
    {
        void* data = (void*)bytes->data;
        if (size) *size = bytes->size;
        cter_free(bytes);
        return data;
    }

    void* data = cter_memdup(bytes->data, bytes->size);
    if (size) *size = bytes->size;
    c_bytes_unref(bytes);
    return data;
}

c_byte_array_t* c_bytes_unref_to_array(c_bytes_t* bytes)
{
    cter_return_val_if_fail(bytes, NULL);
    size_t size = 0u;
    void* data = c_bytes_unref_to_data(bytes, &size);
    return c_byte_array_new_take((uint8_t**)&data, (uint32_t)size);
}

c_bytes_t* c_bytes_new_full(const void* data, size_t size, pfn_cter_func notify, void* usd)
{
    cter_return_val_if_fail(data || size == 0u, NULL);

    c_bytes_t* bytes = cter_malloc0(sizeof(c_bytes_t));
    bytes->data = data;
    bytes->size = size;
    bytes->notify = notify;
    bytes->usd = usd;
    atomic_init(&bytes->ref, 1u);
    return bytes;
}

c_bytes_t* c_bytes_new_take(void** data, size_t size)
{
    cter_return_val_if_fail(data, NULL);
    void* payload = cter_steal_pointer(data);
    return c_bytes_new_full(payload, size, cter_free_wrap, payload);
}

c_bytes_t* c_bytes_new_static(const void* data, size_t size)
{
    cter_return_val_if_fail(data || size == 0u, NULL);
    return c_bytes_new_full(data, size, NULL, NULL);
}

c_bytes_t* c_bytes_new_from_bytes(c_bytes_t* bytes, size_t offset, size_t length)
{
    cter_return_val_if_fail(bytes, NULL);
    cter_return_val_if_fail(offset <= bytes->size, NULL);
    cter_return_val_if_fail(length <= bytes->size - offset, NULL);

    if (offset == 0u && length == bytes->size)
        return c_bytes_ref(bytes);

    return c_bytes_new_full((const uint8_t*)bytes->data + offset, length, c_bytes_unref_wrap, c_bytes_ref(bytes));
}

c_bytes_t* c_bytes_ref(c_bytes_t* bytes)
{
    cter_return_val_if_fail(bytes, NULL);
    atomic_fetch_add_explicit(&bytes->ref, 1u, memory_order_relaxed);
    return bytes;
}

const void* c_bytes_get_data(c_bytes_t* bytes, size_t* size)
{
    cter_return_val_if_fail(bytes, NULL);
    if (size)
        *size = bytes->size;
    return bytes->data;
}

size_t c_bytes_get_size(c_bytes_t* bytes)
{
    cter_return_val_if_fail(bytes, 0u);
    return bytes->size;
}

const void* c_bytes_get_region(c_bytes_t* bytes, size_t element_size, size_t offset, size_t n_elements)
{
    cter_return_val_if_fail(bytes && element_size > 0u, NULL);

    size_t total_size = 0u;
    size_t end_offset = 0u;
    if (!cter_mul_size(element_size, n_elements, &total_size))
        return NULL;
    if (!cter_add_size(offset, total_size, &end_offset))
        return NULL;
    if (end_offset > bytes->size)
        return NULL;
    return (const uint8_t*)bytes->data + offset;
}

bool c_bytes_equal(const c_bytes_t* bytes1, const c_bytes_t* bytes2)
{
    cter_return_val_if_fail(bytes1 && bytes2, false);
    return bytes1->size == bytes2->size && (!(bytes1->size && memcmp(bytes1->data, bytes2->data, bytes1->size)));
}

uint64_t c_bytes_hash(const c_bytes_t* bytes)
{
    cter_return_val_if_fail(bytes, 0u);
    return c_hash_mm3(bytes->data, (uint64_t)bytes->size);
}
#pragma endregion // bytes
#pragma region hash_table
#pragma region hash_functions
uint64_t c_uint32_hash(const void* data)
{
    cter_return_val_if_fail(data, 0u);
    return *(const uint32_t*)data;
}

uint64_t c_uint64_hash(const void* data)
{
    cter_return_val_if_fail(data, 0u);
    return *(const uint64_t*)data;
}

uint64_t c_str_djb_hash(const char* str)
{
    if (C_UNLIKELY(!str))
        return 0u;
    uint64_t hash = 5381u;
    int ch = 0;
    while ((ch = *str++) != 0)
        hash = ((hash << 5) + hash) + (uint8_t)ch;
    return hash;
}

uint64_t c_str_bkdr_hash(const char* str)
{
    if (C_UNLIKELY(!str))
        return 0u;
    uint64_t hash = 0u;
    char ch = 0;
    while ((ch = *str++) != 0)
        hash = hash * 1313u + (uint8_t)ch;
    return hash;
}

uint64_t c_str_sdbm_hash(const char* str)
{
    if (C_UNLIKELY(!str))
        return 0u;
    uint64_t hash = 0u;
    char ch = 0;
    while ((ch = *str++) != 0)
        hash = (uint8_t)ch + (hash << 6) + (hash << 16) - hash;
    return hash;
}

uint64_t c_str_fnv_hash(const char* str)
{
    if (C_UNLIKELY(!str))
        return 0u;
    uint64_t hash = 14695981039346656037ULL;
    int ch = 0;
    while ((ch = *str++) != 0) {
        hash ^= (uint8_t)ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t c_hash_sip64(const void* data, size_t len)
{
#define CTER_U32TO8_LE(p, v)   \
    do {                       \
        (p)[0] = (uint8_t)(v); \
        (p)[1] = (uint8_t)((v) >> 8); \
        (p)[2] = (uint8_t)((v) >> 16); \
        (p)[3] = (uint8_t)((v) >> 24); \
    } while (0)
#define CTER_U64TO8_LE(p, v)                  \
    do {                                      \
        CTER_U32TO8_LE((p), (uint32_t)(v));   \
        CTER_U32TO8_LE((p) + 4, (uint32_t)((v) >> 32)); \
    } while (0)
#define CTER_U8TO64_LE(p) \
    (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) | ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) | ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) | ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56))
#define CTER_ROTL64(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define CTER_SIPROUND     \
    do {                  \
        v0 += v1;         \
        v1 = CTER_ROTL64(v1, 13); \
        v1 ^= v0;         \
        v0 = CTER_ROTL64(v0, 32); \
        v2 += v3;         \
        v3 = CTER_ROTL64(v3, 16); \
        v3 ^= v2;         \
        v0 += v3;         \
        v3 = CTER_ROTL64(v3, 21); \
        v3 ^= v0;         \
        v2 += v1;         \
        v1 = CTER_ROTL64(v1, 17); \
        v1 ^= v2;         \
        v2 = CTER_ROTL64(v2, 32); \
    } while (0)

    const uint8_t* in = (const uint8_t*)data;
    const uint64_t s0 = 31313ULL;
    const uint64_t s1 = 1313131ULL;
    uint64_t k0 = CTER_U8TO64_LE((const uint8_t*)&s0);
    uint64_t k1 = CTER_U8TO64_LE((const uint8_t*)&s1);
    uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ k0;
    uint64_t v1 = UINT64_C(0x646f72616e646f6d) ^ k1;
    uint64_t v2 = UINT64_C(0x6c7967656e657261) ^ k0;
    uint64_t v3 = UINT64_C(0x7465646279746573) ^ k1;
    const uint8_t* end = in + len - (len % sizeof(uint64_t));

    for (; in != end; in += 8) {
        uint64_t m = CTER_U8TO64_LE(in);
        v3 ^= m;
        CTER_SIPROUND;
        CTER_SIPROUND;
        v0 ^= m;
    }

    uint64_t b = ((uint64_t)len) << 56;
    switch (len & 7u) {
    case 7: b |= ((uint64_t)in[6]) << 48; /* fall through */
    case 6: b |= ((uint64_t)in[5]) << 40; /* fall through */
    case 5: b |= ((uint64_t)in[4]) << 32; /* fall through */
    case 4: b |= ((uint64_t)in[3]) << 24; /* fall through */
    case 3: b |= ((uint64_t)in[2]) << 16; /* fall through */
    case 2: b |= ((uint64_t)in[1]) << 8; /* fall through */
    case 1: b |= (uint64_t)in[0]; break;
    default: break;
    }

    v3 ^= b;
    CTER_SIPROUND;
    CTER_SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    CTER_SIPROUND;
    CTER_SIPROUND;
    CTER_SIPROUND;
    CTER_SIPROUND;
    b = v0 ^ v1 ^ v2 ^ v3;

    uint64_t out = 0u;
    CTER_U64TO8_LE((uint8_t*)&out, b);
    return out;

#undef CTER_SIPROUND
#undef CTER_ROTL64
#undef CTER_U8TO64_LE
#undef CTER_U64TO8_LE
#undef CTER_U32TO8_LE
}

uint64_t c_hash_mm3(const void* data, uint64_t len)
{
#define CTER_ROTL32(x, r) (((x) << (r)) | ((x) >> (32 - (r))))
#define CTER_FMIX32(h)    \
    do {                  \
        (h) ^= (h) >> 16; \
        (h) *= 0x85ebca6b; \
        (h) ^= (h) >> 13; \
        (h) *= 0xc2b2ae35; \
        (h) ^= (h) >> 16; \
    } while (0)

    const uint32_t seed = 31313u;
    const uint8_t* bytes = (const uint8_t*)data;
    const int64_t nblocks = (int64_t)(len / 16u);
    uint32_t h1 = seed;
    uint32_t h2 = seed;
    uint32_t h3 = seed;
    uint32_t h4 = seed;
    const uint32_t c1 = 0x239b961b;
    const uint32_t c2 = 0xab0e9789;
    const uint32_t c3 = 0x38b34ae5;
    const uint32_t c4 = 0xa1e38b93;
    const uint32_t* blocks = (const uint32_t*)(bytes + nblocks * 16u);

    for (int64_t i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i * 4 + 0];
        uint32_t k2 = blocks[i * 4 + 1];
        uint32_t k3 = blocks[i * 4 + 2];
        uint32_t k4 = blocks[i * 4 + 3];

        k1 *= c1;
        k1 = CTER_ROTL32(k1, 15);
        k1 *= c2;
        h1 ^= k1;
        h1 = CTER_ROTL32(h1, 19);
        h1 += h2;
        h1 = h1 * 5 + 0x561ccd1b;

        k2 *= c2;
        k2 = CTER_ROTL32(k2, 16);
        k2 *= c3;
        h2 ^= k2;
        h2 = CTER_ROTL32(h2, 17);
        h2 += h3;
        h2 = h2 * 5 + 0x0bcaa747;

        k3 *= c3;
        k3 = CTER_ROTL32(k3, 17);
        k3 *= c4;
        h3 ^= k3;
        h3 = CTER_ROTL32(h3, 15);
        h3 += h4;
        h3 = h3 * 5 + 0x96cd1c35;

        k4 *= c4;
        k4 = CTER_ROTL32(k4, 18);
        k4 *= c1;
        h4 ^= k4;
        h4 = CTER_ROTL32(h4, 13);
        h4 += h1;
        h4 = h4 * 5 + 0x32ac3b17;
    }

    const uint8_t* tail = bytes + nblocks * 16u;
    uint32_t k1 = 0u;
    uint32_t k2 = 0u;
    uint32_t k3 = 0u;
    uint32_t k4 = 0u;

    switch (len & 15u) {
    case 15: k4 ^= ((uint32_t)tail[14]) << 16; /* fall through */
    case 14: k4 ^= ((uint32_t)tail[13]) << 8; /* fall through */
    case 13: k4 ^= ((uint32_t)tail[12]) << 0; k4 *= c4; k4 = CTER_ROTL32(k4, 18); k4 *= c1; h4 ^= k4; /* fall through */
    case 12: k3 ^= ((uint32_t)tail[11]) << 24; /* fall through */
    case 11: k3 ^= ((uint32_t)tail[10]) << 16; /* fall through */
    case 10: k3 ^= ((uint32_t)tail[9]) << 8; /* fall through */
    case 9:  k3 ^= ((uint32_t)tail[8]) << 0; k3 *= c3; k3 = CTER_ROTL32(k3, 17); k3 *= c4; h3 ^= k3; /* fall through */
    case 8:  k2 ^= ((uint32_t)tail[7]) << 24; /* fall through */
    case 7:  k2 ^= ((uint32_t)tail[6]) << 16; /* fall through */
    case 6:  k2 ^= ((uint32_t)tail[5]) << 8; /* fall through */
    case 5:  k2 ^= ((uint32_t)tail[4]) << 0; k2 *= c2; k2 = CTER_ROTL32(k2, 16); k2 *= c3; h2 ^= k2; /* fall through */
    case 4:  k1 ^= ((uint32_t)tail[3]) << 24; /* fall through */
    case 3:  k1 ^= ((uint32_t)tail[2]) << 16; /* fall through */
    case 2:  k1 ^= ((uint32_t)tail[1]) << 8; /* fall through */
    case 1:  k1 ^= ((uint32_t)tail[0]) << 0; k1 *= c1; k1 = CTER_ROTL32(k1, 15); k1 *= c2; h1 ^= k1; /* fall through */
    default: break;
    }

    h1 ^= (uint32_t)len;
    h2 ^= (uint32_t)len;
    h3 ^= (uint32_t)len;
    h4 ^= (uint32_t)len;

    h1 += h2 + h3 + h4;
    h2 += h1;
    h3 += h1;
    h4 += h1;

    CTER_FMIX32(h1);
    CTER_FMIX32(h2);
    CTER_FMIX32(h3);
    CTER_FMIX32(h4);

    h1 += h2 + h3 + h4;
    h2 += h1;
    h3 += h1;
    h4 += h1;

    return (((uint64_t)h2) << 32) | h1;

#undef CTER_FMIX32
#undef CTER_ROTL32
}

#pragma region xxhash

#define CTER_XXH_PRIME_1 11400714785074694791ULL
#define CTER_XXH_PRIME_2 14029467366897019727ULL
#define CTER_XXH_PRIME_3 1609587929392839161ULL
#define CTER_XXH_PRIME_4 9650029242287828579ULL
#define CTER_XXH_PRIME_5 2870177450012600261ULL

static uint64_t cter_xxh_read64(const void* memptr)
{
    uint64_t val = 0u;
    memcpy(&val, memptr, sizeof(val));
    return val;
}

static uint32_t cter_xxh_read32(const void* memptr)
{
    uint32_t val = 0u;
    memcpy(&val, memptr, sizeof(val));
    return val;
}

static uint64_t cter_xxh_rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

uint64_t c_hash_xxh3(const void* data, size_t len)
{
    const uint64_t seed = 31u;
    const uint8_t* p = (const uint8_t*)data;
    const uint8_t* const end = p + len;
    uint64_t h64 = 0u;

    if (len >= 32u) {
        const uint8_t* const limit = end - 32u;
        uint64_t v1 = seed + CTER_XXH_PRIME_1 + CTER_XXH_PRIME_2;
        uint64_t v2 = seed + CTER_XXH_PRIME_2;
        uint64_t v3 = seed + 0u;
        uint64_t v4 = seed - CTER_XXH_PRIME_1;

        do {
            v1 += cter_xxh_read64(p) * CTER_XXH_PRIME_2;
            v1 = cter_xxh_rotl64(v1, 31);
            v1 *= CTER_XXH_PRIME_1;

            v2 += cter_xxh_read64(p + 8u) * CTER_XXH_PRIME_2;
            v2 = cter_xxh_rotl64(v2, 31);
            v2 *= CTER_XXH_PRIME_1;

            v3 += cter_xxh_read64(p + 16u) * CTER_XXH_PRIME_2;
            v3 = cter_xxh_rotl64(v3, 31);
            v3 *= CTER_XXH_PRIME_1;

            v4 += cter_xxh_read64(p + 24u) * CTER_XXH_PRIME_2;
            v4 = cter_xxh_rotl64(v4, 31);
            v4 *= CTER_XXH_PRIME_1;

            p += 32u;
        } while (p <= limit);

        h64 = cter_xxh_rotl64(v1, 1) + cter_xxh_rotl64(v2, 7) + cter_xxh_rotl64(v3, 12) + cter_xxh_rotl64(v4, 18);

        v1 *= CTER_XXH_PRIME_2;
        v1 = cter_xxh_rotl64(v1, 31);
        v1 *= CTER_XXH_PRIME_1;
        h64 ^= v1;
        h64 = h64 * CTER_XXH_PRIME_1 + CTER_XXH_PRIME_4;

        v2 *= CTER_XXH_PRIME_2;
        v2 = cter_xxh_rotl64(v2, 31);
        v2 *= CTER_XXH_PRIME_1;
        h64 ^= v2;
        h64 = h64 * CTER_XXH_PRIME_1 + CTER_XXH_PRIME_4;

        v3 *= CTER_XXH_PRIME_2;
        v3 = cter_xxh_rotl64(v3, 31);
        v3 *= CTER_XXH_PRIME_1;
        h64 ^= v3;
        h64 = h64 * CTER_XXH_PRIME_1 + CTER_XXH_PRIME_4;

        v4 *= CTER_XXH_PRIME_2;
        v4 = cter_xxh_rotl64(v4, 31);
        v4 *= CTER_XXH_PRIME_1;
        h64 ^= v4;
        h64 = h64 * CTER_XXH_PRIME_1 + CTER_XXH_PRIME_4;
    } else {
        h64 = seed + CTER_XXH_PRIME_5;
    }

    h64 += (uint64_t)len;

    while (p + 8u <= end) {
        uint64_t k1 = cter_xxh_read64(p);
        k1 *= CTER_XXH_PRIME_2;
        k1 = cter_xxh_rotl64(k1, 31);
        k1 *= CTER_XXH_PRIME_1;
        h64 ^= k1;
        h64 = cter_xxh_rotl64(h64, 27) * CTER_XXH_PRIME_1 + CTER_XXH_PRIME_4;
        p += 8u;
    }

    if (p + 4u <= end) {
        h64 ^= (uint64_t)cter_xxh_read32(p) * CTER_XXH_PRIME_1;
        h64 = cter_xxh_rotl64(h64, 23) * CTER_XXH_PRIME_2 + CTER_XXH_PRIME_3;
        p += 4u;
    }

    while (p < end) {
        h64 ^= (uint64_t)(*p) * CTER_XXH_PRIME_5;
        h64 = cter_xxh_rotl64(h64, 11) * CTER_XXH_PRIME_1;
        ++p;
    }

    h64 ^= h64 >> 33;
    h64 *= CTER_XXH_PRIME_2;
    h64 ^= h64 >> 29;
    h64 *= CTER_XXH_PRIME_3;
    h64 ^= h64 >> 32;
    return h64;
}

#pragma endregion // xxhash
#pragma endregion // hash_functions
#pragma region hash_table_private
typedef struct _c_hash_table {
    size_t size, mask, count, occupied;
    uint64_t* hashes;
    void** keys;
    void** values;
    int64_t *lru_prev, *lru_next;
    int64_t lru_head, lru_tail;
    pfn_cter_hash hash_func;
    pfn_cter_equal equal_func;
    pfn_cter_notify key_destroy_func, value_destroy_func;
    atomic_uint_fast32_t ref;
} c_hash_table_t;

typedef struct _c_hash_table_iter {
    c_hash_table_t* table;
    int64_t next;
} c_hash_table_iter_t;

static size_t cter_hash_mix64(uint64_t value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return (size_t)value;
}

static uint64_t cter_hash_normalize(uint64_t hash)
{
    if (hash < 2u)
        hash += 2u;
    return hash;
}

static bool cter_hash_keys_equal(const c_hash_table_t* table, const void* lhs, const void* rhs)
{
    if (table->equal_func)
        return table->equal_func(lhs, rhs);
    return lhs == rhs;
}

static size_t cter_hash_bucket_index(const c_hash_table_t* table, uint64_t hash)
{
    return cter_hash_mix64(hash) & table->mask;
}

static void cter_hash_lru_clear(const c_hash_table_t* table, size_t index)
{
    table->lru_prev[index] = -1;
    table->lru_next[index] = -1;
}

static void cter_hash_lru_unlink(c_hash_table_t* table, size_t index)
{
    const int64_t prev = table->lru_prev[index];
    const int64_t next = table->lru_next[index];

    if (prev >= 0)
        table->lru_next[prev] = next;
    else
        table->lru_head = next;

    if (next >= 0)
        table->lru_prev[next] = prev;
    else
        table->lru_tail = prev;

    cter_hash_lru_clear(table, index);
}

static void cter_hash_lru_link_head(c_hash_table_t* table, size_t index)
{
    cter_hash_lru_clear(table, index);
    table->lru_next[index] = table->lru_head;
    if (table->lru_head >= 0)
        table->lru_prev[table->lru_head] = (int64_t)index;
    else
        table->lru_tail = (int64_t)index;
    table->lru_head = (int64_t)index;
}

static void cter_hash_lru_touch(c_hash_table_t* table, size_t index)
{
    if (table->lru_head == (int64_t)index)
        return;

    if (table->lru_prev[index] >= 0 || table->lru_next[index] >= 0 || table->lru_head == (int64_t)index)
        cter_hash_lru_unlink(table, index);

    cter_hash_lru_link_head(table, index);
}

static void cter_hash_table_allocate_storage(c_hash_table_t* table, size_t size)
{
    table->size = size;
    table->mask = size - 1u;
    table->count = 0u;
    table->occupied = 0u;
    table->hashes = cter_malloc0(size * sizeof(uint64_t));
    table->keys = cter_malloc0(size * sizeof(void*));
    table->values = cter_malloc0(size * sizeof(void*));
    table->lru_prev = cter_malloc0(size * sizeof(int64_t));
    table->lru_next = cter_malloc0(size * sizeof(int64_t));
    table->lru_head = -1;
    table->lru_tail = -1;
    for (size_t i = 0u; i < size; ++i)
        cter_hash_lru_clear(table, i);
}

static void cter_hash_table_free_storage(c_hash_table_t* table)
{
    cter_free(table->hashes);
    cter_free(table->keys);
    cter_free(table->values);
    cter_free(table->lru_prev);
    cter_free(table->lru_next);
    memset(table, 0, offsetof(c_hash_table_t, lru_head));
    table->lru_head = -1;
    table->lru_tail = -1;
}

static size_t cter_hash_table_find_slot(const c_hash_table_t* table, const void* key, uint64_t hash, bool* found)
{
    size_t index = cter_hash_bucket_index(table, hash);
    size_t step = 0u;
    size_t first_tombstone = SIZE_MAX;

    while (true) {
        const uint64_t slot_hash = table->hashes[index];

        if (slot_hash == 0u) {
            *found = false;
            return first_tombstone != SIZE_MAX ? first_tombstone : index;
        }

        if (slot_hash == 1u) {
            if (first_tombstone == SIZE_MAX)
                first_tombstone = index;
        } else if (slot_hash == hash && cter_hash_keys_equal(table, table->keys[index], key)) {
            *found = true;
            return index;
        }

        step += 1u;
        index = (index + step) & table->mask;
    }
}

static void cter_hash_table_place_entry(c_hash_table_t* table, void* key, void* value, uint64_t hash)
{
    bool found = false;
    size_t slot = cter_hash_table_find_slot(table, key, hash, &found);
    if (table->hashes[slot] == 0u)
        table->occupied += 1u;
    table->hashes[slot] = hash;
    table->keys[slot] = key;
    table->values[slot] = value;
    table->count += 1u;
    cter_hash_lru_link_head(table, slot);
}

static void cter_hash_table_destroy_slot(c_hash_table_t* table, size_t slot, bool destroy_key, pfn_cter_notify value_destroy_override)
{
    void* key = table->keys[slot];
    void* value = table->values[slot];

    if (destroy_key && table->key_destroy_func && key)
        table->key_destroy_func(key);

    if (value_destroy_override) {
        if (value)
            value_destroy_override(value);
    }
    else if (table->value_destroy_func && value)
        table->value_destroy_func(value);
}

static void cter_hash_table_rehash(c_hash_table_t* table, size_t new_size)
{
    uint64_t* old_hashes = table->hashes;
    void** old_keys = table->keys;
    void** old_values = table->values;
    int64_t* old_lru_prev = table->lru_prev;
    int64_t* old_lru_next = table->lru_next;
    int64_t old_lru_tail = table->lru_tail;

    cter_hash_table_allocate_storage(table, new_size);
    for (int64_t slot = old_lru_tail; slot >= 0; slot = old_lru_prev[slot])
        cter_hash_table_place_entry(table, old_keys[slot], old_values[slot], old_hashes[slot]);

    cter_free(old_hashes);
    cter_free(old_keys);
    cter_free(old_values);
    cter_free(old_lru_prev);
    cter_free(old_lru_next);
}

static void cter_hash_table_maybe_grow(c_hash_table_t* table)
{
    if ((table->occupied + 1u) * 4u >= table->size * 3u) {
        cter_hash_table_rehash(table, table->size * 2u);
        return;
    }

    if (table->occupied > table->count * 2u + 8u)
        cter_hash_table_rehash(table, table->size);
}

static void cter_hash_table_maybe_shrink(c_hash_table_t* table)
{
    if (table->size > 16u && table->count * 8u < table->size) {
        cter_hash_table_rehash(table, table->size / 2u);
        return;
    }

    if (table->occupied > table->count * 2u + 8u)
        cter_hash_table_rehash(table, table->size);
}
#pragma endregion // hash_table_private
void c_hash_table_free(c_hash_table_t* table) {
    c_hash_table_remove_all(table);
    cter_hash_table_free_storage(table);
    cter_free(table);
}

void c_hash_table_unref(c_hash_table_t* table)
{
    if (!table || 1u < atomic_fetch_sub_explicit(&table->ref, 1u, memory_order_acq_rel))
        return;
    c_hash_table_free(table);
}

/**
 * c_hash_table_new:
 * @hash_func: hash function used for bucket selection
 * @value_destroy_func: destroy function for values, or %NULL
 *
 * Creates a new hash table using pointer identity for key equality.
 *
 * Returns: a new #c_hash_table_t
 */
c_hash_table_t* c_hash_table_new(pfn_cter_hash hash_func, pfn_cter_notify value_destroy_func)
{
    return c_hash_table_new_full(hash_func, NULL, NULL, value_destroy_func);
}
/**
 * c_hash_table_new_full:
 * @hash_func: hash function used for bucket selection
 * @key_equal_func: equality function, or %NULL to use pointer identity
 * @key_destroy_func: destroy function for keys, or %NULL
 * @value_destroy_func: destroy function for values, or %NULL
 *
 * Creates a new hash table. Ownership of inserted keys and values is
 * transferred to the table.
 *
 * Returns: a new #c_hash_table_t
 */
c_hash_table_t* c_hash_table_new_full(pfn_cter_hash hash_func, pfn_cter_equal key_equal_func, pfn_cter_notify key_destroy_func, pfn_cter_notify value_destroy_func)
{
    cter_return_val_if_fail(hash_func, NULL);

    c_hash_table_t* table = cter_malloc0(sizeof(c_hash_table_t));
    table->hash_func = hash_func;
    table->equal_func = key_equal_func;
    table->key_destroy_func = key_destroy_func;
    table->value_destroy_func = value_destroy_func;
    atomic_init(&table->ref, 1u);
    cter_hash_table_allocate_storage(table, 16u);
    return table;
}

c_hash_table_t* c_hash_table_ref(c_hash_table_t* table)
{
    cter_return_val_if_fail(table, NULL);
    atomic_fetch_add_explicit(&table->ref, 1u, memory_order_relaxed);
    return table;
}

bool c_hash_table_contains(c_hash_table_t* table, const void* key)
{
    cter_return_val_if_fail(table, false);

    const uint64_t hash = cter_hash_normalize(table->hash_func(key));
    bool found = false;
    (void)cter_hash_table_find_slot(table, key, hash, &found);
    return found;
}

uint64_t c_hash_table_get_count(c_hash_table_t* table)
{
    cter_return_val_if_fail(table, 0u);
    return (uint64_t)table->count;
}
/**
 * c_hash_table_insert:
 * @table: a #c_hash_table_t
 * @key: key to insert
 * @value: value to insert
 *
 * Inserts or replaces a key/value pair. If an equal key already exists, the
 * old key and value are destroyed using the table destroy notifiers and the
 * new pair takes ownership.
 *
 * Returns: %true on success
 */
bool c_hash_table_insert(c_hash_table_t* table, void* key, void* value)
{
    cter_return_val_if_fail(table, false);

    cter_hash_table_maybe_grow(table);

    const uint64_t hash = cter_hash_normalize(table->hash_func(key));
    bool found = false;
    size_t slot = cter_hash_table_find_slot(table, key, hash, &found);

    if (found) {
        void* old_key = table->keys[slot];
        void* old_value = table->values[slot];
        table->keys[slot] = key;
        table->values[slot] = value;
        table->hashes[slot] = hash;
        cter_hash_lru_touch(table, slot);
        if (table->key_destroy_func && old_key && old_key != key)
            table->key_destroy_func(old_key);
        if (table->value_destroy_func && old_value && old_value != value)
            table->value_destroy_func(old_value);
        return true;
    }

    if (table->hashes[slot] == 0u)
        table->occupied += 1u;

    table->hashes[slot] = hash;
    table->keys[slot] = key;
    table->values[slot] = value;
    table->count += 1u;
    cter_hash_lru_link_head(table, slot);
    return true;
}

void* c_hash_table_lookup(c_hash_table_t* table, const void* key)
{
    cter_return_val_if_fail(table, NULL);

    const uint64_t hash = cter_hash_normalize(table->hash_func(key));
    bool found = false;
    size_t slot = cter_hash_table_find_slot(table, key, hash, &found);
    if (!found)
        return NULL;

    cter_hash_lru_touch(table, slot);
    return table->values[slot];
}

void c_hash_table_remove_all(c_hash_table_t* table)
{
    cter_return_if_fail(table);
    for (size_t i = 0u; i < table->size; ++i) {
        if (table->hashes[i] >= 2u)
            cter_hash_table_destroy_slot(table, i, true, NULL);
        table->hashes[i] = 0u;
        table->keys[i] = NULL;
        table->values[i] = NULL;
        cter_hash_lru_clear(table, i);
    }
    table->count = 0u;
    table->occupied = 0u;
    table->lru_head = -1;
    table->lru_tail = -1;
}

bool c_hash_table_remove(c_hash_table_t* table, const void* key, pfn_cter_notify value_destroy_override)
{
    cter_return_val_if_fail(table, false);

    const uint64_t hash = cter_hash_normalize(table->hash_func(key));
    bool found = false;
    size_t slot = cter_hash_table_find_slot(table, key, hash, &found);
    if (!found)
        return false;

    cter_hash_table_destroy_slot(table, slot, true, value_destroy_override);
    cter_hash_lru_unlink(table, slot);
    table->hashes[slot] = 1u;
    table->keys[slot] = NULL;
    table->values[slot] = NULL;
    table->count -= 1u;
    cter_hash_table_maybe_shrink(table);
    return true;
}

bool c_hash_table_steal(c_hash_table_t* table, const void* key)
{
    cter_return_val_if_fail(table, false);

    const uint64_t hash = cter_hash_normalize(table->hash_func(key));
    bool found = false;
    size_t slot = cter_hash_table_find_slot(table, key, hash, &found);
    if (!found)
        return false;

    cter_hash_lru_unlink(table, slot);
    table->hashes[slot] = 1u;
    table->keys[slot] = NULL;
    table->values[slot] = NULL;
    table->count -= 1u;
    cter_hash_table_maybe_shrink(table);
    return true;
}

#pragma region hash_table_iter
void c_hash_table_iter_free(c_hash_table_iter_t* iter)
{
    if (!iter)
        return;
    c_hash_table_unref(iter->table);
    cter_free(iter);
}

c_hash_table_iter_t* c_hash_table_iter_new(c_hash_table_t* table)
{
    cter_return_val_if_fail(table, NULL);
    c_hash_table_iter_t* iter = cter_malloc0(sizeof(c_hash_table_iter_t));
    iter->table = c_hash_table_ref(table);
    iter->next = table->lru_head;
    return iter;
}

bool c_hash_table_iter_next(c_hash_table_iter_t* iter, const void** key, void** value)
{
    cter_return_val_if_fail(iter && iter->table, false);
    if (iter->next < 0)
        return false;

    const size_t slot = (size_t)iter->next;
    iter->next = iter->table->lru_next[slot];
    if (key)
        *key = iter->table->keys[slot];
    if (value)
        *value = iter->table->values[slot];
    return true;
}
#pragma endregion // hash_table_iter
#pragma endregion // hash_table
#pragma region ptr_array

static bool cter_ptr_array_expand(c_ptr_array_t* array, uint32_t extra)
{
    uint32_t needed = 0u;
    cter_return_val_if_fail(array, false);
    if (!cter_add_u32(array->len, extra, &needed))
        return false;
    if (needed > UINT32_MAX - 1u)
        return false;
    needed += 1u;

    if (needed <= array->capacity)
        return true;

    size_t bytes = cter_nearest_pow2((size_t)needed * sizeof(void*));
    if (bytes < 16u)
        bytes = 16u;

    const uint32_t old_capacity = array->capacity;
    void** pdata = cter_realloc(array->pdata, bytes);
    if (!pdata)
        return false;
    array->pdata = pdata;
    array->capacity = (uint32_t)(bytes / sizeof(void*));
    if (array->capacity > old_capacity)
        cter_zero_bytes(array->pdata + old_capacity, (size_t)(array->capacity - old_capacity) * sizeof(void*));
    return true;
}

void** c_ptr_array_free(c_ptr_array_t* arr, bool free_segment)
{
    cter_return_val_if_fail(arr, NULL);
    void** res = NULL;
    if (free_segment) {
        if (arr->notify) {
            for (uint32_t i = 0u; i < arr->len; ++i) {
                if (arr->pdata[i])
                    arr->notify(arr->pdata[i], arr->usd);
            }
        }
        cter_free(arr->pdata);
    }
    else
        res = arr->pdata;
    cter_free(arr);
    return res;
}

void c_ptr_array_unref(c_ptr_array_t* arr)
{
    if (!arr || 1u < atomic_fetch_sub_explicit(&arr->ref, 1u, memory_order_acq_rel)) return;
    c_ptr_array_free(arr, true);
}

c_ptr_array_t* c_ptr_array_new_full(uint32_t reserved_size, pfn_cter_func notify, void* usd)
{
    c_ptr_array_t* arr = cter_malloc0(sizeof(c_ptr_array_t));
    arr->notify = notify;
    arr->usd = usd;
    if (reserved_size)
        cter_ptr_array_expand(arr, reserved_size);

    atomic_init(&arr->ref, 1u);
    return arr;
}

c_ptr_array_t* c_ptr_array_new_take(void** data, uint32_t len, pfn_cter_func notify, void* usd)
{
    cter_return_val_if_fail(data && *data && len, NULL);
    c_ptr_array_t* arr = c_ptr_array_new_full(0u, notify, usd);
    arr->pdata = cter_steal_pointer((void**)data);
    arr->len = len;
    arr->capacity = len;
    return arr;
}

c_ptr_array_t* c_ptr_array_new_from_array(void** data, uint32_t len, pfn_cter_copy copy_func, void* copy_func_usd, pfn_cter_func notify, void* usd)
{
    cter_return_val_if_fail(data && *data && len, NULL);
    c_ptr_array_t* arr = c_ptr_array_new_full(len, notify, usd);

    if (copy_func) {
        for (uint32_t i = 0u; i < len; ++i)
            arr->pdata[i] = copy_func(data[i], copy_func_usd);
    }
    else if (len)
        memcpy(arr->pdata, data, len * sizeof(void*));
    
    arr->len = len;
    return arr;
}

c_ptr_array_t* c_ptr_array_new(void)
{
    return c_ptr_array_new_full(0u, NULL, NULL);
}

c_ptr_array_t* c_ptr_array_ref(c_ptr_array_t* arr)
{
    cter_return_val_if_fail(arr, NULL);
    atomic_fetch_add_explicit(&arr->ref, 1u, memory_order_relaxed);
    return arr;
}

c_ptr_array_t* c_ptr_array_copy(c_ptr_array_t* arr, pfn_cter_copy copy_func, void* usd)
{
    cter_return_val_if_fail(arr, NULL);
    c_ptr_array_t* copy = c_ptr_array_new_full(arr->len, arr->notify, arr->usd);
    cter_return_val_if_fail(copy, NULL);
    for (uint32_t i = 0u; i < arr->len; ++i)
        copy->pdata[i] = copy_func ? copy_func(arr->pdata[i], usd) : arr->pdata[i];
    copy->len = arr->len;
    return copy;
}

void* c_ptr_array_exchange_index(c_ptr_array_t* arr, uint32_t idx, void* value, bool free_segment)
{
    cter_return_val_if_fail(arr && idx < arr->len, NULL);
    void* old = arr->pdata[idx];
    if (free_segment && arr->notify && old)
        arr->notify(old, arr->usd);
    arr->pdata[idx] = value;
    return old;
}

void c_ptr_array_extend_and_steal(c_ptr_array_t* array_to_extend, c_ptr_array_t* arr)
{
    cter_return_if_fail(array_to_extend && arr);
    c_ptr_array_extend(array_to_extend, arr, NULL, NULL);
    cter_free(arr->pdata);
    memset(arr, 0, offsetof(c_ptr_array_t, notify));
}

void c_ptr_array_extend(c_ptr_array_t* array_to_extend, c_ptr_array_t* arr, pfn_cter_copy copy_func, void* usd)
{
    cter_return_if_fail(array_to_extend && arr);
    if (C_UNLIKELY(!(arr->len && cter_ptr_array_expand(array_to_extend, arr->len))))
        return;
    for (uint32_t i = 0u; i < arr->len; i++)
        array_to_extend->pdata[array_to_extend->len + i] = copy_func ? copy_func(arr->pdata[i], usd) : arr->pdata[i];
    array_to_extend->len += arr->len;
    array_to_extend->pdata[array_to_extend->len] = NULL;
}

bool c_ptr_array_find(c_ptr_array_t* haystack, const void* needle, uint32_t* idx)
{
    return c_ptr_array_find_with_equal_func(haystack, needle, NULL, idx);
}

bool c_ptr_array_find_with_equal_func(c_ptr_array_t* haystack, const void* needle, pfn_cter_equal equal_func, uint32_t* idx)
{
    cter_return_val_if_fail(haystack, false);
    if (!equal_func)
        equal_func = c_direct_equal;
    for (uint32_t i = 0u; i < haystack->len; ++i) {
        if (equal_func(haystack->pdata[i], needle)) {
            if (idx)
                *idx = i;
            return true;
        }
    }
    return false;
}

void c_ptr_array_foreach(c_ptr_array_t* arr, pfn_cter_func func, void* usd)
{
    cter_return_if_fail(arr && func);
    for (uint32_t i = 0u; i < arr->len; ++i)
        func(arr->pdata[i], usd);
}

void c_ptr_array_insert(c_ptr_array_t* arr, uint32_t idx, void* data)
{
    cter_return_if_fail(arr);
    if (idx >= arr->len) {
        c_ptr_array_push(arr, data);
        return;
    }
    if (!cter_ptr_array_expand(arr, 1u))
        return;
    memmove(arr->pdata + idx + 1u, arr->pdata + idx, (size_t)(arr->len - idx) * sizeof(void*));
    arr->pdata[idx] = data;
    arr->len += 1u;
    arr->pdata[arr->len] = NULL;
}


void c_ptr_array_push(c_ptr_array_t* arr, void* data)
{
    cter_return_if_fail(arr);
    if (C_UNLIKELY(!cter_ptr_array_expand(arr, 1u)))
        return;
    arr->pdata[arr->len++] = data;
    arr->pdata[arr->len] = NULL;
}

void* c_ptr_array_remove_index(c_ptr_array_t* arr, uint32_t idx)
{
    cter_return_val_if_fail(arr && idx < arr->len, NULL);
    void* old = arr->pdata[idx];
    if (arr->notify && old)
        arr->notify(old, arr->usd);
    if (idx + 1u < arr->len)
        memmove(arr->pdata + idx, arr->pdata + idx + 1u, (size_t)(arr->len - idx - 1u) * sizeof(void*));
    arr->len -= 1u;
    arr->pdata[arr->len] = NULL;
    return old;
}

void c_ptr_array_remove_all(c_ptr_array_t* arr, pfn_cter_func notify, void* usd)
{
    cter_return_if_fail(arr);
    if (!notify)
        notify = arr->notify;
    if (notify) {
        for (uint32_t i = 0u; i < arr->len; i++) {
            if (arr->pdata[i])
                notify(arr->pdata[i], usd);
        }
    }
    cter_zero_bytes(arr->pdata, (size_t)arr->capacity * sizeof(void*));
    arr->len = 0u;
}

void c_ptr_array_set_free_func(c_ptr_array_t* arr, pfn_cter_func notify, void* usd)
{
    cter_return_if_fail(arr);
    arr->notify = notify;
    arr->usd = usd;
}

void** c_ptr_array_steal(c_ptr_array_t* arr, uint32_t* len)
{
    cter_return_val_if_fail(arr, NULL);
    if (len)
        *len = arr->len;
    void** data = cter_steal_pointer((void**)&arr->pdata);
    arr->len = 0u;
    arr->capacity = 0u;
    return data;
}

void* c_ptr_array_steal_index(c_ptr_array_t* arr, uint32_t idx)
{
    cter_return_val_if_fail(arr && idx < arr->len, NULL);
    void* old_value = arr->pdata[idx];
    if (idx + 1u < arr->len)
        memmove(arr->pdata + idx, arr->pdata + idx + 1u, (size_t)(arr->len - idx - 1u) * sizeof(void*));
    arr->len -= 1u;
    arr->pdata[arr->len] = NULL;
    return old_value;
}

void** c_ptr_array_steal_all(c_ptr_array_t* arr)
{
    cter_return_val_if_fail(arr, NULL);
    void** data = cter_steal_pointer((void**)&arr->pdata);
    arr->len = 0u;
    arr->capacity = 0u;
    return data;
}

void* c_ptr_array_swap_index(c_ptr_array_t* arr, uint32_t idx, void* data)
{
    cter_return_val_if_fail(arr && idx < arr->len, NULL);
    void* old = arr->pdata[idx];
    arr->pdata[idx] = data;
    return old;
}

#pragma endregion // ptr_array
#pragma region queue
#pragma region queue_private
static c_queue_link_t* cter_queue_link_last(c_queue_link_t* link)
{
    while (link && link->next)
        link = link->next;
    return link;
}

static c_queue_link_t* cter_queue_link_first(c_queue_link_t* link)
{
    while (link && link->prev)
        link = link->prev;
    return link;
}

static void cter_queue_link_chain_free(c_queue_link_t* link)
{
    while (link) {
        c_queue_link_t* next = link->next;
        cter_free(link);
        link = next;
    }
}

static void cter_queue_link_chain_free_full(c_queue_link_t* link, pfn_cter_notify notify)
{
    while (link) {
        c_queue_link_t* next = link->next;
        if (notify && link->data)
            notify(link->data);
        cter_free(link);
        link = next;
    }
}

static c_queue_link_t* cter_queue_link_nth(c_queue_link_t* link, uint32_t n)
{
    while (link && n--)
        link = link->next;
    return link;
}

static int cter_queue_link_index_of(c_queue_link_t* head, const void* data)
{
    int index = 0;
    while (head) {
        if (head->data == data)
            return index;
        head = head->next;
        ++index;
    }
    return -1;
}

static int cter_queue_link_position(c_queue_link_t* head, c_queue_link_t* needle)
{
    int index = 0;
    while (head) {
        if (head == needle)
            return index;
        head = head->next;
        ++index;
    }
    return -1;
}

static c_queue_link_t* cter_queue_link_find(c_queue_link_t* head, const void* data)
{
    while (head) {
        if (head->data == data)
            return head;
        head = head->next;
    }
    return NULL;
}

static c_queue_link_t* cter_queue_link_find_custom(c_queue_link_t* head, const void* data, pfn_cter_compare compare_func)
{
    while (head) {
        if (compare_func(head->data, data) == 0)
            return head;
        head = head->next;
    }
    return NULL;
}

static c_queue_link_t* cter_queue_link_remove(c_queue_link_t* head, c_queue_link_t* link)
{
    if (!link)
        return head;

    if (link->prev)
        link->prev->next = link->next;
    if (link->next)
        link->next->prev = link->prev;
    if (head == link)
        head = link->next;

    link->prev = NULL;
    link->next = NULL;
    return head;
}

static c_queue_link_t* cter_queue_link_insert_before(c_queue_link_t* head, c_queue_link_t* sibling, c_queue_link_t* link)
{
    cter_return_val_if_fail(link, head);
    cter_return_val_if_true(link->prev || link->next, head);

    if (!head)
        return link;

    if (!sibling) {
        c_queue_link_t* tail = cter_queue_link_last(head);
        tail->next = link;
        link->prev = tail;
        return head;
    }

    link->prev = sibling->prev;
    link->next = sibling;
    sibling->prev = link;
    if (link->prev)
        link->prev->next = link;
    return head == sibling ? link : head;
}

static c_queue_link_t* cter_queue_link_append(c_queue_link_t* head, void* data)
{
    c_queue_link_t* link = c_queue_link_new(data);
    return cter_queue_link_insert_before(head, NULL, link);
}

static c_queue_link_t* cter_queue_link_prepend(c_queue_link_t* head, void* data)
{
    c_queue_link_t* link = c_queue_link_new(data);
    return cter_queue_link_insert_before(head, head, link);
}

static c_queue_link_t* cter_queue_link_reverse(c_queue_link_t* head)
{
    c_queue_link_t* last = NULL;
    while (head) {
        last = head;
        head = head->next;
        last->next = last->prev;
        last->prev = head;
    }
    return last;
}

static c_queue_link_t* cter_queue_merge(c_queue_link_t* a, c_queue_link_t* b, pfn_cter_compare_data compare_func, void* usd)
{
    c_queue_link_t dummy = { 0 };
    c_queue_link_t* tail = &dummy;

    while (a && b) {
        if (compare_func(a->data, b->data, usd) <= 0) {
            tail->next = a;
            a->prev = tail == &dummy ? NULL : tail;
            a = a->next;
        } else {
            tail->next = b;
            b->prev = tail == &dummy ? NULL : tail;
            b = b->next;
        }
        tail = tail->next;
    }

    tail->next = a ? a : b;
    if (tail->next)
        tail->next->prev = tail == &dummy ? NULL : tail;

    c_queue_link_t* head = dummy.next;
    c_queue_link_t* prev = NULL;
    while (head) {
        head->prev = prev;
        prev = head;
        if (!head->next)
            break;
        head = head->next;
    }
    return dummy.next;
}

static c_queue_link_t* cter_queue_link_sort(c_queue_link_t* head, pfn_cter_compare_data compare_func, void* usd)
{
    if (!head || !head->next)
        return head;

    c_queue_link_t* slow = head;
    c_queue_link_t* fast = head->next;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }

    c_queue_link_t* second = slow->next;
    slow->next = NULL;
    if (second)
        second->prev = NULL;

    c_queue_link_t* left = cter_queue_link_sort(head, compare_func, usd);
    c_queue_link_t* right = cter_queue_link_sort(second, compare_func, usd);
    return cter_queue_merge(left, right, compare_func, usd);
}
#pragma endregion // queue_private
#pragma region queue_link
void c_queue_link_free(c_queue_link_t* link)
{
    cter_return_if_fail(link && !link->prev && !link->next);
    cter_free(link);
}

c_queue_link_t* c_queue_link_new(void* data)
{
    c_queue_link_t* link = cter_malloc0(sizeof(c_queue_link_t));
    link->data = data;
    return link;
}

void* c_queue_link_get_data(c_queue_link_t* link)
{
    cter_return_val_if_fail(link, NULL);
    return link->data;
}

c_queue_link_t* c_queue_link_get_prev(c_queue_link_t* link)
{
    cter_return_val_if_fail(link, NULL);
    return link->prev;
}

c_queue_link_t* c_queue_link_get_next(c_queue_link_t* link)
{
    cter_return_val_if_fail(link, NULL);
    return link->next;
}

void c_queue_link_set_data(c_queue_link_t* link, void* data)
{
    cter_return_if_fail(link);
    link->data = data;
}
#pragma endregion // queue_link

void c_queue_free(c_queue_t* queue)
{
    if (!queue)
        return;
    cter_queue_link_chain_free(queue->head);
    cter_free(queue);
}

void c_queue_free_full(c_queue_t* queue, pfn_cter_notify notify)
{
    if (!queue)
        return;
    cter_queue_link_chain_free_full(queue->head, notify);
    cter_free(queue);
}

void c_queue_clear(c_queue_t* queue)
{
    cter_return_if_fail(queue);
    cter_queue_link_chain_free(queue->head);
    c_queue_init(queue);
}

void c_queue_clear_full(c_queue_t* queue, pfn_cter_notify notify)
{
    cter_return_if_fail(queue);
    cter_queue_link_chain_free_full(queue->head, notify);
    c_queue_init(queue);
}

void c_queue_init(c_queue_t* queue)
{
    cter_return_if_fail(queue);
    queue->head = NULL;
    queue->tail = NULL;
    queue->length = 0u;
}

c_queue_t* c_queue_new(void)
{
    return cter_malloc0(sizeof(c_queue_t));
}

c_queue_t* c_queue_copy(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    c_queue_t* copy = c_queue_new();
    for (c_queue_link_t* node = queue->head; node; node = node->next)
        c_queue_push_tail(copy, node->data);
    return copy;
}

void c_queue_delete_link(c_queue_t* queue, c_queue_link_t* link)
{
    cter_return_if_fail(queue && link);
    c_queue_unlink(queue, link);
    cter_free(link);
}

c_queue_link_t* c_queue_find(c_queue_t* queue, const void* data)
{
    cter_return_val_if_fail(queue, NULL);
    return cter_queue_link_find(queue->head, data);
}

c_queue_link_t* c_queue_find_custom(c_queue_t* queue, const void* data, pfn_cter_compare compare_func)
{
    cter_return_val_if_fail(queue && compare_func, NULL);
    return cter_queue_link_find_custom(queue->head, data, compare_func);
}

void c_queue_foreach(c_queue_t* queue, pfn_cter_func func, void* usd)
{
    cter_return_if_fail(queue && func);
    c_queue_link_t* node = queue->head;
    while (node) {
        c_queue_link_t* next = node->next;
        func(node->data, usd);
        node = next;
    }
}

int c_queue_index(c_queue_t* queue, const void* data)
{
    cter_return_val_if_fail(queue, -1);
    return cter_queue_link_index_of(queue->head, data);
}

void c_queue_insert_after(c_queue_t* queue, c_queue_link_t* sibling, void* data)
{
    cter_return_if_fail(queue);
    if (!sibling)
        c_queue_push_head(queue, data);
    else
        c_queue_insert_before(queue, sibling->next, data);
}

void c_queue_insert_after_link(c_queue_t* queue, c_queue_link_t* sibling, c_queue_link_t* link)
{
    cter_return_if_fail(queue && link);
    if (!sibling)
        c_queue_push_head_link(queue, link);
    else
        c_queue_insert_before_link(queue, sibling->next, link);
}

void c_queue_insert_before(c_queue_t* queue, c_queue_link_t* sibling, void* data)
{
    cter_return_if_fail(queue);
    c_queue_link_t* link = c_queue_link_new(data);
    c_queue_insert_before_link(queue, sibling, link);
}

void c_queue_insert_before_link(c_queue_t* queue, c_queue_link_t* sibling, c_queue_link_t* link)
{
    cter_return_if_fail(queue && link);
    cter_return_if_true(link->prev || link->next);

    queue->head = cter_queue_link_insert_before(queue->head, sibling, link);
    if (!link->next)
        queue->tail = link;
    else if (!queue->tail)
        queue->tail = cter_queue_link_last(queue->head);
    queue->head = cter_queue_link_first(queue->head);
    queue->length += 1u;
}

void c_queue_insert_sorted(c_queue_t* queue, void* data, pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(queue && compare_func);
    c_queue_link_t* link = queue->head;
    while (link && compare_func(link->data, data, usd) < 0)
        link = link->next;
    c_queue_insert_before(queue, link, data);
}

int c_queue_link_index(c_queue_t* queue, c_queue_link_t* link)
{
    cter_return_val_if_fail(queue, -1);
    return cter_queue_link_position(queue->head, link);
}

void c_queue_push_head(c_queue_t* queue, void* data)
{
    cter_return_if_fail(queue);
    queue->head = cter_queue_link_prepend(queue->head, data);
    if (!queue->tail)
        queue->tail = queue->head;
    queue->length += 1u;
}

void c_queue_push_head_link(c_queue_t* queue, c_queue_link_t* link)
{
    cter_return_if_fail(queue && link);
    cter_return_if_true(link->prev || link->next);

    link->next = queue->head;
    link->prev = NULL;
    if (queue->head)
        queue->head->prev = link;
    else
        queue->tail = link;
    queue->head = link;
    queue->length += 1u;
}

void c_queue_push_tail(c_queue_t* queue, void* data)
{
    cter_return_if_fail(queue);
    queue->head = cter_queue_link_append(queue->head, data);
    queue->tail = cter_queue_link_last(queue->head);
    queue->length += 1u;
}

void c_queue_push_tail_link(c_queue_t* queue, c_queue_link_t* link)
{
    cter_return_if_fail(queue && link);
    cter_return_if_true(link->prev || link->next);

    link->prev = queue->tail;
    link->next = NULL;
    if (queue->tail)
        queue->tail->next = link;
    else
        queue->head = link;
    queue->tail = link;
    queue->length += 1u;
}

void c_queue_push_nth(c_queue_t* queue, void* data, int n)
{
    cter_return_if_fail(queue);
    if (n < 0 || (uint32_t)n >= queue->length) {
        c_queue_push_tail(queue, data);
        return;
    }
    c_queue_insert_before(queue, c_queue_peek_nth_link(queue, (uint32_t)n), data);
}

void c_queue_push_nth_link(c_queue_t* queue, int n, c_queue_link_t* link)
{
    cter_return_if_fail(queue && link);
    if (n < 0 || (uint32_t)n >= queue->length) {
        c_queue_push_tail_link(queue, link);
        return;
    }
    c_queue_insert_before_link(queue, c_queue_peek_nth_link(queue, (uint32_t)n), link);
}

void* c_queue_pop_head(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    c_queue_link_t* link = c_queue_pop_head_link(queue);
    if (!link)
        return NULL;
    void* data = link->data;
    cter_free(link);
    return data;
}

c_queue_link_t* c_queue_pop_head_link(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    if (!queue->head)
        return NULL;

    c_queue_link_t* link = queue->head;
    queue->head = link->next;
    if (queue->head)
        queue->head->prev = NULL;
    else
        queue->tail = NULL;
    link->prev = NULL;
    link->next = NULL;
    queue->length -= 1u;
    return link;
}

void* c_queue_pop_tail(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    c_queue_link_t* link = c_queue_pop_tail_link(queue);
    if (!link)
        return NULL;
    void* data = link->data;
    cter_free(link);
    return data;
}

c_queue_link_t* c_queue_pop_tail_link(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    if (!queue->tail)
        return NULL;

    c_queue_link_t* link = queue->tail;
    queue->tail = link->prev;
    if (queue->tail)
        queue->tail->next = NULL;
    else
        queue->head = NULL;
    link->prev = NULL;
    link->next = NULL;
    queue->length -= 1u;
    return link;
}

void* c_queue_pop_nth(c_queue_t* queue, uint32_t n)
{
    cter_return_val_if_fail(queue, NULL);
    c_queue_link_t* link = c_queue_pop_nth_link(queue, n);
    if (!link)
        return NULL;
    void* data = link->data;
    cter_free(link);
    return data;
}

c_queue_link_t* c_queue_pop_nth_link(c_queue_t* queue, uint32_t n)
{
    cter_return_val_if_fail(queue, NULL);
    if (n >= queue->length)
        return NULL;
    c_queue_link_t* link = c_queue_peek_nth_link(queue, n);
    c_queue_unlink(queue, link);
    return link;
}

void* c_queue_peek_head(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    return queue->head ? queue->head->data : NULL;
}

c_queue_link_t* c_queue_peek_head_link(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    return queue->head;
}

void* c_queue_peek_tail(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    return queue->tail ? queue->tail->data : NULL;
}

c_queue_link_t* c_queue_peek_tail_link(c_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    return queue->tail;
}

void* c_queue_peek_nth(c_queue_t* queue, uint32_t n)
{
    cter_return_val_if_fail(queue, NULL);
    c_queue_link_t* link = c_queue_peek_nth_link(queue, n);
    return link ? link->data : NULL;
}

c_queue_link_t* c_queue_peek_nth_link(c_queue_t* queue, uint32_t n)
{
    cter_return_val_if_fail(queue, NULL);
    if (n >= queue->length)
        return NULL;

    if (n > queue->length / 2u) {
        uint32_t back_index = queue->length - n - 1u;
        c_queue_link_t* link = queue->tail;
        while (back_index--)
            link = link->prev;
        return link;
    }

    return cter_queue_link_nth(queue->head, n);
}

bool c_queue_remove(c_queue_t* queue, const void* data)
{
    cter_return_val_if_fail(queue, false);
    c_queue_link_t* link = cter_queue_link_find(queue->head, data);
    if (!link)
        return false;
    c_queue_delete_link(queue, link);
    return true;
}

uint32_t c_queue_remove_all(c_queue_t* queue, const void* data)
{
    cter_return_val_if_fail(queue, 0u);
    uint32_t removed = 0u;
    c_queue_link_t* link = queue->head;
    while (link) {
        c_queue_link_t* next = link->next;
        if (link->data == data) {
            c_queue_delete_link(queue, link);
            removed += 1u;
        }
        link = next;
    }
    return removed;
}

void c_queue_reverse(c_queue_t* queue)
{
    cter_return_if_fail(queue);
    queue->tail = queue->head;
    queue->head = cter_queue_link_reverse(queue->head);
}

void c_queue_sort(c_queue_t* queue, pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(queue && compare_func);
    queue->head = cter_queue_link_sort(queue->head, compare_func, usd);
    queue->tail = cter_queue_link_last(queue->head);
}

void c_queue_unlink(c_queue_t* queue, c_queue_link_t* link)
{
    cter_return_if_fail(queue && link);
    if (link == queue->tail)
        queue->tail = link->prev;
    queue->head = cter_queue_link_remove(queue->head, link);
    if (!queue->head)
        queue->tail = NULL;
    queue->length -= 1u;
}

#pragma region async_queue

typedef struct _cter_async_sort_data {
    pfn_cter_compare_data compare_func;
    void* usd;
} cter_async_sort_data_t;

typedef struct _c_async_queue {
    c_queue_t queue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pfn_cter_notify notify;
    void* usd;
    atomic_uint_fast32_t waiting;
    atomic_uint_fast32_t ref;
} c_async_queue_t;

typedef enum _cter_async_wait_mode {
    CTER_ASYNC_NO_WAIT = 0,
    CTER_ASYNC_WAIT_FOREVER = 1,
    CTER_ASYNC_WAIT_TIMEOUT = 2
} cter_async_wait_mode_e;

static int cter_async_queue_invert_compare(const void* a, const void* b, void* usd)
{
    cter_async_sort_data_t* data = (cter_async_sort_data_t*)usd;
    return -data->compare_func(a, b, data->usd);
}

static bool cter_cond_wait_relative(pthread_cond_t* cond, pthread_mutex_t* mutex, uint64_t timeout_us)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    ts.tv_sec += (time_t)(timeout_us / 1000000u);
    ts.tv_nsec += (long)((timeout_us % 1000000u) * 1000u);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    return pthread_cond_timedwait(cond, mutex, &ts) == 0;
}

static void* cter_async_queue_pop_unlocked_core(c_async_queue_t* queue, cter_async_wait_mode_e mode, uint64_t timeout_us)
{
    if (!c_queue_peek_tail_link(&queue->queue) && mode != CTER_ASYNC_NO_WAIT) {
        atomic_fetch_add_explicit(&queue->waiting, 1u, memory_order_relaxed);
        while (!c_queue_peek_tail_link(&queue->queue)) {
            if (mode == CTER_ASYNC_WAIT_FOREVER) {
                if (pthread_cond_wait(&queue->cond, &queue->mutex) != 0)
                    break;
            } else if (!cter_cond_wait_relative(&queue->cond, &queue->mutex, timeout_us)) {
                break;
            }
        }
        atomic_fetch_sub_explicit(&queue->waiting, 1u, memory_order_relaxed);
    }

    return c_queue_pop_tail(&queue->queue);
}

void c_async_queue_free(c_async_queue_t* queue) {
    cter_return_if_true(atomic_load_explicit(&queue->waiting, memory_order_acquire) != 0u);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
    if (queue->notify)
        c_queue_clear_full(&queue->queue, queue->notify);
    else
        c_queue_clear(&queue->queue);
    cter_free(queue);
}

void c_async_queue_unref(c_async_queue_t* queue)
{
    cter_return_if_fail(queue);
    if (!queue || 1u < atomic_fetch_sub_explicit(&queue->ref, 1u, memory_order_acq_rel))
        return;
    c_async_queue_free(queue);
}

c_async_queue_t* c_async_queue_new_full(pfn_cter_notify item_free_func)
{
    c_async_queue_t* queue = cter_malloc0(sizeof(c_async_queue_t));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    c_queue_init(&queue->queue);
    queue->notify = item_free_func;
    atomic_init(&queue->waiting, 0u);
    atomic_init(&queue->ref, 1u);
    return queue;
}

c_async_queue_t* c_async_queue_new(void)
{
    return c_async_queue_new_full(NULL);
}

c_async_queue_t* c_async_queue_ref(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    atomic_fetch_add_explicit(&queue->ref, 1u, memory_order_relaxed);
    return queue;
}

void c_async_queue_clear(c_async_queue_t* queue)
{
    cter_return_if_fail(queue);
    pthread_mutex_lock(&queue->mutex);
    c_async_queue_clear_unlocked(queue);
    pthread_mutex_unlock(&queue->mutex);
}

void c_async_queue_clear_unlocked(c_async_queue_t* queue)
{
    cter_return_if_fail(queue);
    if (queue->notify)
        c_queue_clear_full(&queue->queue, queue->notify);
    else
        c_queue_clear(&queue->queue);
}

void* c_async_queue_get_mutex(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    return &queue->mutex;
}

void c_async_queue_lock(c_async_queue_t* queue)
{
    cter_return_if_fail(queue);
    pthread_mutex_lock(&queue->mutex);
}

int c_async_queue_length(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, 0);
    pthread_mutex_lock(&queue->mutex);
    int length = (int)queue->queue.length - (int)atomic_load_explicit(&queue->waiting, memory_order_relaxed);
    pthread_mutex_unlock(&queue->mutex);
    return length;
}

int c_async_queue_length_unlocked(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, 0);
    return (int)queue->queue.length - (int)atomic_load_explicit(&queue->waiting, memory_order_relaxed);
}

void c_async_queue_unlock(c_async_queue_t* queue)
{
    cter_return_if_fail(queue);
    pthread_mutex_unlock(&queue->mutex);
}

void c_async_queue_push(c_async_queue_t* queue, void* data)
{
    cter_return_if_fail(queue && data);
    pthread_mutex_lock(&queue->mutex);
    c_async_queue_push_unlocked(queue, data);
    pthread_mutex_unlock(&queue->mutex);
}

void c_async_queue_push_unlocked(c_async_queue_t* queue, void* data)
{
    cter_return_if_fail(queue && data);
    c_queue_push_head(&queue->queue, data);
    if (atomic_load_explicit(&queue->waiting, memory_order_acquire))
        pthread_cond_signal(&queue->cond);
}

void c_async_queue_push_front(c_async_queue_t* queue, void* item)
{
    cter_return_if_fail(queue && item);
    pthread_mutex_lock(&queue->mutex);
    c_async_queue_push_front_unlocked(queue, item);
    pthread_mutex_unlock(&queue->mutex);
}

void c_async_queue_push_front_unlocked(c_async_queue_t* queue, void* item)
{
    cter_return_if_fail(queue && item);
    c_queue_push_tail(&queue->queue, item);
    if (atomic_load_explicit(&queue->waiting, memory_order_acquire))
        pthread_cond_signal(&queue->cond);
}

void c_async_queue_push_sorted(c_async_queue_t* queue, void* data, pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(queue && data && compare_func);
    pthread_mutex_lock(&queue->mutex);
    c_async_queue_push_sorted_unlocked(queue, data, compare_func, usd);
    pthread_mutex_unlock(&queue->mutex);
}

void c_async_queue_push_sorted_unlocked(c_async_queue_t* queue, void* data, pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(queue && data && compare_func);
    cter_async_sort_data_t sort_data = {
        .compare_func = compare_func,
        .usd = usd,
    };
    c_queue_insert_sorted(&queue->queue, data, cter_async_queue_invert_compare, &sort_data);
    if (atomic_load_explicit(&queue->waiting, memory_order_acquire))
        pthread_cond_signal(&queue->cond);
}

void* c_async_queue_pop(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    pthread_mutex_lock(&queue->mutex);
    void* data = cter_async_queue_pop_unlocked_core(queue, CTER_ASYNC_WAIT_FOREVER, 0u);
    pthread_mutex_unlock(&queue->mutex);
    return data;
}

void* c_async_queue_pop_unlocked(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    return cter_async_queue_pop_unlocked_core(queue, CTER_ASYNC_WAIT_FOREVER, 0u);
}

void* c_async_queue_try_pop(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    pthread_mutex_lock(&queue->mutex);
    void* data = cter_async_queue_pop_unlocked_core(queue, CTER_ASYNC_NO_WAIT, 0u);
    pthread_mutex_unlock(&queue->mutex);
    return data;
}

void* c_async_queue_try_pop_unlocked(c_async_queue_t* queue)
{
    cter_return_val_if_fail(queue, NULL);
    return cter_async_queue_pop_unlocked_core(queue, CTER_ASYNC_NO_WAIT, 0u);
}

void* c_async_queue_timeout_pop(c_async_queue_t* queue, uint64_t timeout_us)
{
    cter_return_val_if_fail(queue, NULL);
    pthread_mutex_lock(&queue->mutex);
    void* data = cter_async_queue_pop_unlocked_core(queue, CTER_ASYNC_WAIT_TIMEOUT, timeout_us);
    pthread_mutex_unlock(&queue->mutex);
    return data;
}

void* c_async_queue_timeout_pop_unlocked(c_async_queue_t* queue, uint64_t timeout_us)
{
    cter_return_val_if_fail(queue, NULL);
    return cter_async_queue_pop_unlocked_core(queue, CTER_ASYNC_WAIT_TIMEOUT, timeout_us);
}

bool c_async_queue_remove(c_async_queue_t* queue, void* item)
{
    cter_return_val_if_fail(queue && item, false);
    pthread_mutex_lock(&queue->mutex);
    bool removed = c_async_queue_remove_unlocked(queue, item);
    pthread_mutex_unlock(&queue->mutex);
    return removed;
}

bool c_async_queue_remove_unlocked(c_async_queue_t* queue, void* item)
{
    cter_return_val_if_fail(queue && item, false);
    return c_queue_remove(&queue->queue, item);
}

void c_async_queue_sort(c_async_queue_t* queue, pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(queue && compare_func);
    pthread_mutex_lock(&queue->mutex);
    c_async_queue_sort_unlocked(queue, compare_func, usd);
    pthread_mutex_unlock(&queue->mutex);
}

void c_async_queue_sort_unlocked(c_async_queue_t* queue, pfn_cter_compare_data compare_func, void* usd)
{
    cter_return_if_fail(queue && compare_func);
    cter_async_sort_data_t sort_data = {
        .compare_func = compare_func,
        .usd = usd,
    };
    c_queue_sort(&queue->queue, cter_async_queue_invert_compare, &sort_data);
}

#pragma endregion // async_queue
#pragma endregion // queue
#pragma endregion // containers
