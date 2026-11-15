#ifndef ZN_SSL_CTX_INTERNAL_H
#define ZN_SSL_CTX_INTERNAL_H

#include <openssl/ssl.h>

struct zn_ssl_ctx {
    SSL_CTX *ctx;
};

#endif
