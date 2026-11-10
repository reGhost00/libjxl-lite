#ifndef C_CTER_H
#define C_CTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma region misc
#pragma region alloc
typedef struct _c_allocator {
    void* (*malloc)(size_t size);
    void* (*realloc)(void* ptr, size_t size);
    void (*free)(void* ptr);
} c_allocator_t;

bool c_set_allocator(const c_allocator_t* alloc);

#pragma endregion // alloc
typedef void (*pfn_cter_notify)(void* data);
typedef void (*pfn_cter_func)(void* data, void* usd);
typedef void* (*pfn_cter_copy)(const void* data, void* usd);
typedef int (*pfn_cter_compare)(const void* a, const void* b);
typedef int (*pfn_cter_compare_data)(const void* a, const void* b, void* usd);
typedef bool (*pfn_cter_equal)(const void* a, const void* b);
typedef uint64_t (*pfn_cter_hash)(const void* key);

bool c_direct_equal(const void* a, const void* b);
bool c_str_equal(const void* a, const void* b);
#pragma endregion // misc
#pragma region containers
#pragma region array

typedef struct _c_array {
    uint8_t* data;
    uint32_t len;
    uint32_t capacity;
    uint32_t element_size;
    atomic_uint_fast32_t ref;
} c_array_t;

#define c_array_append_val(array, value) c_array_append_vals((array), &(value), 1u)
#define c_array_prepend_val(array, value) c_array_prepend_vals((array), &(value), 1u)
#define c_array_insert_val(array, index, value) c_array_insert_vals((array), (index), &(value), 1u)
#define c_array_index(array, type, index) (((type*)(array)->data)[(index)])

uint8_t* c_array_free(c_array_t* array, bool free_segment);
void c_array_unref(c_array_t* array);
c_array_t* c_array_ref(c_array_t* array);

c_array_t* c_array_new_full(uint32_t reserved_size, uint32_t element_size);
c_array_t* c_array_new_take(void* data, uint32_t len, uint32_t element_size);
c_array_t* c_array_new(uint32_t element_size);

void* c_array_steal(c_array_t* array, uint32_t* len);
c_array_t* c_array_copy(c_array_t* array);
c_array_t* c_array_append_vals(c_array_t* array, const void* data, uint32_t len);
c_array_t* c_array_prepend_vals(c_array_t* array, const void* data, uint32_t len);
c_array_t* c_array_insert_vals(c_array_t* array, uint32_t index, const void* data, uint32_t len);
c_array_t* c_array_remove_index(c_array_t* array, uint32_t index);
c_array_t* c_array_remove_range(c_array_t* array, uint32_t index, uint32_t length);
void c_array_remove_all(c_array_t* array);
c_array_t* c_array_set_size(c_array_t* array, uint32_t length);
void c_array_sort(c_array_t* array, pfn_cter_compare compare_func);
void c_array_sort_with_data(c_array_t* array, pfn_cter_compare_data compare_func, void* usd);
bool c_array_binary_search(c_array_t* array, const void* target, pfn_cter_compare compare_func, uint32_t* out_match_index);

#pragma endregion // array
#pragma region // byte_array

typedef struct _c_byte_array {
    uint8_t* data;
    uint32_t len;
    uint32_t capacity;
    atomic_uint_fast32_t ref;
} c_byte_array_t;

typedef struct _c_bytes c_bytes_t;

uint8_t* c_byte_array_free(c_byte_array_t* arr, bool free_segment);
c_bytes_t* c_byte_array_free_to_bytes(c_byte_array_t* array);
void c_byte_array_unref(c_byte_array_t* array);
c_byte_array_t* c_byte_array_ref(c_byte_array_t* array);
c_byte_array_t* c_byte_array_new(void);
c_byte_array_t* c_byte_array_sized_new(uint32_t reserved_size);
c_byte_array_t* c_byte_array_new_take(uint8_t** data, uint32_t len);

c_byte_array_t* c_byte_array_append(c_byte_array_t* array, const uint8_t* data, uint32_t len);
c_byte_array_t* c_byte_array_prepend(c_byte_array_t* array, const uint8_t* data, uint32_t len);
c_byte_array_t* c_byte_array_remove_index(c_byte_array_t* array, uint32_t index);
c_byte_array_t* c_byte_array_remove_range(c_byte_array_t* array, uint32_t index, uint32_t length);
c_byte_array_t* c_byte_array_set_size(c_byte_array_t* array, uint32_t length);
uint8_t* c_byte_array_steal(c_byte_array_t* array, uint32_t* len);
void c_byte_array_sort(c_byte_array_t* array, pfn_cter_compare compare_func);
void c_byte_array_sort_with_data(c_byte_array_t* array, pfn_cter_compare_data compare_func, void* usd);
c_bytes_t* c_byte_array_to_bytes(c_byte_array_t* array);
#pragma endregion // byte_array
#pragma region bytes
void c_bytes_free(c_bytes_t* bytes);
void* c_bytes_free_to_data(c_bytes_t* bytes, size_t* size);
void c_bytes_unref(c_bytes_t* bytes);
void* c_bytes_unref_to_data(c_bytes_t* bytes, size_t* size);
c_byte_array_t* c_bytes_unref_to_array(c_bytes_t* bytes);

c_bytes_t* c_bytes_new_full(const void* data, size_t size, pfn_cter_func notify, void* usd);
c_bytes_t* c_bytes_new_take(void** data, size_t size);
c_bytes_t* c_bytes_new_static(const void* data, size_t size);
c_bytes_t* c_bytes_new_from_bytes(c_bytes_t* bytes, size_t offset, size_t length);
c_bytes_t* c_bytes_ref(c_bytes_t* bytes);

const void* c_bytes_get_data(c_bytes_t* bytes, size_t* size);
const void* c_bytes_get_region(c_bytes_t* bytes, size_t element_size, size_t offset, size_t n_elements);
size_t c_bytes_get_size(c_bytes_t* bytes);
uint64_t c_bytes_hash(const c_bytes_t* bytes);
bool c_bytes_equal(const c_bytes_t* bytes1, const c_bytes_t* bytes2);

#pragma endregion // bytes
#pragma region hash_table
#pragma region hash_functions
uint64_t c_uint32_hash(const void* data);
uint64_t c_uint64_hash(const void* data);
uint64_t c_str_djb_hash(const char* str);
uint64_t c_str_bkdr_hash(const char* str);
uint64_t c_str_sdbm_hash(const char* str);
uint64_t c_str_fnv_hash(const char* str);
uint64_t c_hash_sip64(const void* data, size_t len);
uint64_t c_hash_mm3(const void* data, uint64_t len);
uint64_t c_hash_xxh3(const void* data, size_t len);
#pragma endregion // hash_functions
/**
 * c_hash_table_t:
 *
 * Opaque hash table type. The implementation stores full key/value pairs in an
 * open-addressed (closed-hash) table and promotes touched entries to the head
 * of an internal LRU order on successful insert/lookup.
 */
typedef struct _c_hash_table c_hash_table_t;
void c_hash_table_free(c_hash_table_t* table);
void c_hash_table_unref(c_hash_table_t* table);

c_hash_table_t* c_hash_table_new(pfn_cter_hash hash_func, pfn_cter_notify value_destroy_func);
c_hash_table_t* c_hash_table_new_full(pfn_cter_hash hash_func, pfn_cter_equal key_equal_func, pfn_cter_notify key_destroy_func, pfn_cter_notify value_destroy_func);
c_hash_table_t* c_hash_table_ref(c_hash_table_t* table);

bool c_hash_table_contains(c_hash_table_t* table, const void* key);
uint64_t c_hash_table_get_count(c_hash_table_t* table);
bool c_hash_table_insert(c_hash_table_t* table, void* key, void* value);
void* c_hash_table_lookup(c_hash_table_t* table, const void* key);
void c_hash_table_remove_all(c_hash_table_t* table);
bool c_hash_table_remove(c_hash_table_t* table, const void* key, pfn_cter_notify value_destroy_override);
bool c_hash_table_steal(c_hash_table_t* table, const void* key);
#pragma region hash_table_iter
/**
 * c_hash_table_iter_t:
 *
 * Heap-allocated iterator for #c_hash_table_t. Use c_hash_table_iter_new(),
 * c_hash_table_iter_next(), and c_hash_table_iter_free(). Iteration walks
 * entries from most recently used to least recently used.
 */
typedef struct _c_hash_table_iter c_hash_table_iter_t;
void c_hash_table_iter_free(c_hash_table_iter_t* iter);
c_hash_table_iter_t* c_hash_table_iter_new(c_hash_table_t* table);
bool c_hash_table_iter_next(c_hash_table_iter_t* iter, const void** key, void** value);
#pragma endregion // hash_table_iter
#pragma endregion // hash_table
#pragma region ptr_array
typedef struct _c_ptr_array {
    void** pdata;
    uint32_t len;
    uint32_t capacity;
    pfn_cter_func notify;
    void* usd;
    atomic_uint_fast32_t ref;
} c_ptr_array_t;

#define c_ptr_array_index(arr, idx) (arr->pdata[(idx)])

void** c_ptr_array_free(c_ptr_array_t* arr, bool free_segment);
void c_ptr_array_unref(c_ptr_array_t* arr);

c_ptr_array_t* c_ptr_array_new_full(uint32_t reserved_size, pfn_cter_func notify, void* usd);
c_ptr_array_t* c_ptr_array_new_take(void** data, uint32_t len, pfn_cter_func notify, void* usd);
c_ptr_array_t* c_ptr_array_new_from_array(void** data, uint32_t len, pfn_cter_copy copy_func, void* copy_func_usd, pfn_cter_func notify, void* usd);
c_ptr_array_t* c_ptr_array_new(void);
c_ptr_array_t* c_ptr_array_ref(c_ptr_array_t* arr);

c_ptr_array_t* c_ptr_array_copy(c_ptr_array_t* arr, pfn_cter_copy copy_func, void* usd);
void* c_ptr_array_exchange_index(c_ptr_array_t* arr, uint32_t idx, void* value, bool free_segment);
void c_ptr_array_extend_and_steal(c_ptr_array_t* array_to_extend, c_ptr_array_t* arr);
void c_ptr_array_extend(c_ptr_array_t* array_to_extend, c_ptr_array_t* arr, pfn_cter_copy copy_func, void* usd);
bool c_ptr_array_find(c_ptr_array_t* haystack, const void* needle, uint32_t* idx);
bool c_ptr_array_find_with_equal_func(c_ptr_array_t* haystack, const void* needle, pfn_cter_equal equal_func, uint32_t* idx);
void c_ptr_array_foreach(c_ptr_array_t* arr, pfn_cter_func func, void* usd);
void c_ptr_array_insert(c_ptr_array_t* arr, uint32_t idx, void* data);
void c_ptr_array_push(c_ptr_array_t* arr, void* data);
void* c_ptr_array_remove_index(c_ptr_array_t* arr, uint32_t idx);
void c_ptr_array_remove_all(c_ptr_array_t* arr, pfn_cter_func notify, void* usd);
void c_ptr_array_set_free_func(c_ptr_array_t* arr, pfn_cter_func notify, void* usd);
void** c_ptr_array_steal(c_ptr_array_t* arr, uint32_t* len);
void* c_ptr_array_steal_index(c_ptr_array_t* arr, uint32_t idx);
void** c_ptr_array_steal_all(c_ptr_array_t* arr);
void* c_ptr_array_swap_index(c_ptr_array_t* arr, uint32_t idx, void* data);

#pragma endregion // ptr_array
#pragma region queue
#pragma region queue_link

typedef struct _c_queue_link {
    struct _c_queue_link* prev;
    struct _c_queue_link* next;
    void* data;
} c_queue_link_t;

void c_queue_link_free(c_queue_link_t* link);
c_queue_link_t* c_queue_link_new(void* data);

void* c_queue_link_get_data(c_queue_link_t* link);
c_queue_link_t* c_queue_link_get_prev(c_queue_link_t* link);
c_queue_link_t* c_queue_link_get_next(c_queue_link_t* link);
void c_queue_link_set_data(c_queue_link_t* link, void* data);

#pragma endregion // queue_link
typedef struct _c_queue {
    c_queue_link_t* head;
    c_queue_link_t* tail;
    uint32_t length;
} c_queue_t;

#define C_QUEUE_INIT { NULL, NULL, 0u }

void c_queue_free(c_queue_t* queue);
void c_queue_free_full(c_queue_t* queue, pfn_cter_notify notify);
void c_queue_clear(c_queue_t* queue);
void c_queue_clear_full(c_queue_t* queue, pfn_cter_notify notify);
void c_queue_init(c_queue_t* queue);
c_queue_t* c_queue_new(void);

c_queue_t* c_queue_copy(c_queue_t* queue);
void c_queue_delete_link(c_queue_t* queue, c_queue_link_t* link);
c_queue_link_t* c_queue_find(c_queue_t* queue, const void* data);
c_queue_link_t* c_queue_find_custom(c_queue_t* queue, const void* data, pfn_cter_compare compare_func);
void c_queue_foreach(c_queue_t* queue, pfn_cter_func func, void* usd);
int c_queue_index(c_queue_t* queue, const void* data);
void c_queue_insert_after(c_queue_t* queue, c_queue_link_t* sibling, void* data);
void c_queue_insert_after_link(c_queue_t* queue, c_queue_link_t* sibling, c_queue_link_t* link);
void c_queue_insert_before(c_queue_t* queue, c_queue_link_t* sibling, void* data);
void c_queue_insert_before_link(c_queue_t* queue, c_queue_link_t* sibling, c_queue_link_t* link);
void c_queue_insert_sorted(c_queue_t* queue, void* data, pfn_cter_compare_data compare_func, void* usd);
int c_queue_link_index(c_queue_t* queue, c_queue_link_t* link);
void c_queue_push_head(c_queue_t* queue, void* data);
void c_queue_push_head_link(c_queue_t* queue, c_queue_link_t* link);
void c_queue_push_tail(c_queue_t* queue, void* data);
void c_queue_push_tail_link(c_queue_t* queue, c_queue_link_t* link);
void c_queue_push_nth(c_queue_t* queue, void* data, int n);
void c_queue_push_nth_link(c_queue_t* queue, int n, c_queue_link_t* link);
void* c_queue_pop_head(c_queue_t* queue);
c_queue_link_t* c_queue_pop_head_link(c_queue_t* queue);
void* c_queue_pop_tail(c_queue_t* queue);
c_queue_link_t* c_queue_pop_tail_link(c_queue_t* queue);
void* c_queue_pop_nth(c_queue_t* queue, uint32_t n);
c_queue_link_t* c_queue_pop_nth_link(c_queue_t* queue, uint32_t n);
void* c_queue_peek_head(c_queue_t* queue);
c_queue_link_t* c_queue_peek_head_link(c_queue_t* queue);
void* c_queue_peek_tail(c_queue_t* queue);
c_queue_link_t* c_queue_peek_tail_link(c_queue_t* queue);
void* c_queue_peek_nth(c_queue_t* queue, uint32_t n);
c_queue_link_t* c_queue_peek_nth_link(c_queue_t* queue, uint32_t n);
bool c_queue_remove(c_queue_t* queue, const void* data);
uint32_t c_queue_remove_all(c_queue_t* queue, const void* data);
void c_queue_reverse(c_queue_t* queue);
void c_queue_sort(c_queue_t* queue, pfn_cter_compare_data compare_func, void* usd);
void c_queue_unlink(c_queue_t* queue, c_queue_link_t* link);
#pragma region asybc_queue

typedef struct _c_async_queue c_async_queue_t;
void c_async_queue_free(c_async_queue_t* queue);
void c_async_queue_unref(c_async_queue_t* queue);
c_async_queue_t* c_async_queue_new_full(pfn_cter_notify item_free_func);
c_async_queue_t* c_async_queue_new(void);
c_async_queue_t* c_async_queue_ref(c_async_queue_t* queue);

void c_async_queue_clear(c_async_queue_t* queue);
void c_async_queue_clear_unlocked(c_async_queue_t* queue);
void* c_async_queue_get_mutex(c_async_queue_t* queue);
void c_async_queue_lock(c_async_queue_t* queue);
int c_async_queue_length(c_async_queue_t* queue);
int c_async_queue_length_unlocked(c_async_queue_t* queue);
void c_async_queue_unlock(c_async_queue_t* queue);
void c_async_queue_push(c_async_queue_t* queue, void* data);
void c_async_queue_push_unlocked(c_async_queue_t* queue, void* data);
void c_async_queue_push_front(c_async_queue_t* queue, void* item);
void c_async_queue_push_front_unlocked(c_async_queue_t* queue, void* item);
void c_async_queue_push_sorted(c_async_queue_t* queue, void* data, pfn_cter_compare_data compare_func, void* usd);
void c_async_queue_push_sorted_unlocked(c_async_queue_t* queue, void* data, pfn_cter_compare_data compare_func, void* usd);
void* c_async_queue_pop(c_async_queue_t* queue);
void* c_async_queue_pop_unlocked(c_async_queue_t* queue);
void* c_async_queue_try_pop(c_async_queue_t* queue);
void* c_async_queue_try_pop_unlocked(c_async_queue_t* queue);
void* c_async_queue_timeout_pop(c_async_queue_t* queue, uint64_t timeout_us);
void* c_async_queue_timeout_pop_unlocked(c_async_queue_t* queue, uint64_t timeout_us);
bool c_async_queue_remove(c_async_queue_t* queue, void* item);
bool c_async_queue_remove_unlocked(c_async_queue_t* queue, void* item);
void c_async_queue_sort(c_async_queue_t* queue, pfn_cter_compare_data compare_func, void* usd);
void c_async_queue_sort_unlocked(c_async_queue_t* queue, pfn_cter_compare_data compare_func, void* usd);

#pragma endregion // asybc_queue
#pragma endregion // queue
#pragma endregion // containers
#ifdef __cplusplus
}
#endif
#endif // C_CTER_H
