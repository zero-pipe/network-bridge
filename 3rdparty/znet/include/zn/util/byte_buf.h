#ifndef ZN_UTIL_BYTE_BUF_H
#define ZN_UTIL_BYTE_BUF_H

/**
 * 单所有者可扩容字节缓冲：协议解析等 append/clear 场景。
 * 与 zn_buf（引用计数共享载荷）无关。
 */
#include "../zn_export.h"
#include "../zn_errno.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zn_byte_buf zn_byte_buf;

ZN_API zn_byte_buf *zn_byte_buf_create(size_t initial_capacity);
ZN_API void zn_byte_buf_destroy(zn_byte_buf *b);

ZN_API const char *zn_byte_buf_data(const zn_byte_buf *b);
ZN_API size_t zn_byte_buf_size(const zn_byte_buf *b);
ZN_API size_t zn_byte_buf_capacity(const zn_byte_buf *b);

ZN_API zn_err_t zn_byte_buf_reserve(zn_byte_buf *b, size_t capacity);
ZN_API zn_err_t zn_byte_buf_append(zn_byte_buf *b, const void *data, size_t len);
ZN_API zn_err_t zn_byte_buf_clear(zn_byte_buf *b);
/** 丢弃缓冲区前 n 字节 */
ZN_API zn_err_t zn_byte_buf_erase_front(zn_byte_buf *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* ZN_UTIL_BYTE_BUF_H */
