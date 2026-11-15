#ifndef ZN_NET_SSL_H
#define ZN_NET_SSL_H

#include "../zn_export.h"
#include "../zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zn_ssl_ctx zn_ssl_ctx;

/** 客户端 TLS 配置（拉流用） */
typedef struct zn_ssl_config {
    /** PEM CA 文件；空则使用系统默认（若 verify_peer=1 且 OpenSSL 支持） */
    const char *ca_file;
    /** 1=校验服务端证书；0=不校验（仅测试/内网） */
    int verify_peer;
    /** 可选客户端证书 PEM */
    const char *client_cert_file;
    const char *client_key_file;
    /** 服务端模式：加载 cert_file/key_file，使用 TLS_server_method */
    int server_mode;
    const char *cert_file;
    const char *key_file;
} zn_ssl_config_t;

/** 进程级 OpenSSL 初始化（可重复调用，线程安全） */
ZN_API zn_err_t zn_ssl_global_init(void);
ZN_API void zn_ssl_global_fini(void);

ZN_API zn_ssl_ctx *zn_ssl_ctx_create(const zn_ssl_config_t *cfg);
ZN_API void zn_ssl_ctx_destroy(zn_ssl_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZN_NET_SSL_H */
