#ifndef NB_STR_H
#define NB_STR_H

#include <stddef.h>
#include <string.h>

#include <nb/error.h>

/* 安全字符串拷贝：src 为空时用 def，始终 NUL 结尾。 */
static inline void nb_copy_str(char *dst, size_t cap, const char *src, const char *def)
{
    const char *s = (src && src[0]) ? src : def;
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    if (!s) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < cap && s[i]) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
}

/*
 * Checked copy for public name/identity fields. Unlike nb_copy_str, a value
 * that does not fit (including the terminating NUL) is rejected instead of
 * silently truncated. NULL/empty src selects def; def may be NULL for an empty
 * string.
 */
static inline int nb_copy_checked(char *dst, size_t cap, const char *src, const char *def)
{
    const char *s = (src && src[0]) ? src : def;
    size_t n;

    if (!dst || cap == 0) {
        return NB_ERR_INVALID;
    }
    if (!s) {
        dst[0] = '\0';
        return NB_OK;
    }
    n = strlen(s);
    if (n >= cap) {
        return NB_ERR_TOOBIG;
    }
    memcpy(dst, s, n + 1);
    return NB_OK;
}

/* Validate that src (when non-NULL) fits cap including its NUL terminator. */
static inline int nb_str_fits(const char *src, size_t cap)
{
    return (!src || strlen(src) < cap) ? NB_OK : NB_ERR_TOOBIG;
}

#endif /* NB_STR_H */
