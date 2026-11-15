#include "zn/util/byte_buf.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

struct zn_byte_buf {
    char *data;
    size_t size;
    size_t capacity;
};

zn_byte_buf *zn_byte_buf_create(size_t initial_capacity)
{
    zn_byte_buf *b = (zn_byte_buf *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    if (initial_capacity > 0) {
        b->data = (char *)malloc(initial_capacity);
        if (!b->data) {
            free(b);
            return NULL;
        }
        b->capacity = initial_capacity;
    }
    return b;
}

void zn_byte_buf_destroy(zn_byte_buf *b)
{
    if (!b)
        return;
    free(b->data);
    free(b);
}

const char *zn_byte_buf_data(const zn_byte_buf *b)
{
    return b && b->data ? b->data : "";
}

size_t zn_byte_buf_size(const zn_byte_buf *b)
{
    return b ? b->size : 0;
}

size_t zn_byte_buf_capacity(const zn_byte_buf *b)
{
    return b ? b->capacity : 0;
}

zn_err_t zn_byte_buf_reserve(zn_byte_buf *b, size_t capacity)
{
    if (!b)
        return ZN_ERR_INVALID;
    if (capacity <= b->capacity)
        return ZN_OK;
    char *p = (char *)realloc(b->data, capacity);
    if (!p)
        return ZN_ERR_NOMEM;
    b->data = p;
    b->capacity = capacity;
    return ZN_OK;
}

zn_err_t zn_byte_buf_append(zn_byte_buf *b, const void *data, size_t len)
{
    const char *src = (const char *)data;
    ptrdiff_t self_off = -1;

    if (!b || (!data && len))
        return ZN_ERR_INVALID;
    if (len == 0)
        return ZN_OK;

    /*
     * 支持“把缓冲自身的一段追加回自身”（HTTP 分帧在 remain 内回拼剩余字节
     * 就是这个用法）。源指针若落在本缓冲内：
     *   1) 先记偏移，realloc 后按新基址重算，避免悬垂读；
     *   2) 用 memmove 而非 memcpy，避免区间重叠（ASan: memcpy-param-overlap）。
     */
    if (b->data && src >= b->data && src < b->data + b->capacity) {
        self_off = src - b->data;
    }

    size_t new_size = b->size + len;
    size_t need_cap = new_size + 1;
    if (need_cap > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 64;
        while (cap < need_cap)
            cap *= 2;
        zn_err_t err = zn_byte_buf_reserve(b, cap);
        if (err != ZN_OK)
            return err;
    }
    if (self_off >= 0) {
        src = b->data + self_off;
    }
    memmove(b->data + b->size, src, len);
    b->size = new_size;
    b->data[b->size] = '\0';
    return ZN_OK;
}

zn_err_t zn_byte_buf_clear(zn_byte_buf *b)
{
    if (!b)
        return ZN_ERR_INVALID;
    b->size = 0;
    if (b->data)
        b->data[0] = '\0';
    return ZN_OK;
}

zn_err_t zn_byte_buf_erase_front(zn_byte_buf *b, size_t n)
{
    if (!b)
        return ZN_ERR_INVALID;
    if (n == 0)
        return ZN_OK;
    if (n >= b->size) {
        b->size = 0;
        if (b->data)
            b->data[0] = '\0';
        return ZN_OK;
    }
    memmove(b->data, b->data + n, b->size - n);
    b->size -= n;
    if (b->data)
        b->data[b->size] = '\0';
    return ZN_OK;
}
