#ifndef NB_RPC_TYPES_H
#define NB_RPC_TYPES_H

/*
 * RPC-spine types: call metadata, streams, request/response views, opaque
 * handles and transport addressing.
 *
 * Everything here is meaningful only to request/response calls. Shared value
 * types (payloads, errors, log levels, version) live in <nb/base/types.h>.
 */

#include <nb/base/types.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Message flags ---- */
#define NB_MSG_FLAG_FRAMEWORK_STATUS 0x01u

/* ---- Metadata (gRPC-style string key + binary value) ---- */
#define NB_METADATA_MAX_ENTRIES 16
#define NB_METADATA_KEY_MAX 64
#define NB_METADATA_VALUE_MAX 256
#define NB_METADATA_WIRE_MAX 4096
#define NB_STATUS_MSG_MAX 512

typedef uint32_t nb_request_id_t;

typedef struct nb_metadata_entry {
    char key[NB_METADATA_KEY_MAX];
    uint8_t value[NB_METADATA_VALUE_MAX];
    uint16_t value_len;
} nb_metadata_entry_t;

typedef struct nb_metadata {
    uint16_t count;
    nb_metadata_entry_t entries[NB_METADATA_MAX_ENTRIES];
} nb_metadata_t;

void nb_metadata_init(nb_metadata_t *metadata);
int nb_metadata_set(nb_metadata_t *metadata, const char *key, const void *value, size_t len);
int nb_metadata_set_string(nb_metadata_t *metadata, const char *key, const char *value);
const void *nb_metadata_get(const nb_metadata_t *metadata, const char *key, size_t *len);
const char *nb_metadata_get_string(const nb_metadata_t *metadata, const char *key);
int nb_metadata_remove(nb_metadata_t *metadata, const char *key);
int nb_metadata_encode(const nb_metadata_t *metadata, void *buf, size_t cap, size_t *out_len);
int nb_metadata_decode(nb_metadata_t *metadata, const void *buf, size_t len);

/* ---- Opaque handles ---- */
typedef struct nb_node nb_node_t;
typedef struct nb_service nb_service_t;
typedef struct nb_proxy nb_proxy_t;
typedef struct nb_call nb_call_t;
typedef struct nb_stream nb_stream_t;

/* ---- Stream flags and chunks ---- */
#define NB_STREAM_FLAG_FIRST 0x01u
#define NB_STREAM_FLAG_LAST 0x02u
#define NB_STREAM_FLAG_CANCEL 0x04u

typedef struct nb_chunk {
    const void *data;
    size_t len;
    uint64_t offset;
    const nb_metadata_t *metadata; /* may be NULL; callback scope only */
    const char *status_msg;        /* may be NULL */
    uint32_t flags;
    uint8_t encoding;
} nb_chunk_t;

typedef struct nb_stream_meta {
    uint32_t flags;
    uint32_t stream_id;
    uint64_t offset;
    uint64_t total_size;
} nb_stream_meta_t;

/* ---- Transport and node mode ---- */
typedef enum nb_transport_kind {
    NB_TRANSPORT_KIND_UDP = 0,
    NB_TRANSPORT_KIND_TCP = 1,
    NB_TRANSPORT_KIND_CUSTOM = 255
} nb_transport_kind_t;

typedef enum nb_node_mode {
    NB_NODE_MODE_MESH = 0,
    NB_NODE_MODE_STATIC = 1
} nb_node_mode_t;

/* ---- Request/response views ---- */
typedef struct nb_request {
    const char *service;
    const char *method;
    nb_payload_t payload;
    const char *peer_host;
    uint16_t peer_port;
    uint64_t deadline_ms;
    const nb_metadata_t *metadata; /* may be NULL; callback scope only */
} nb_request_t;

typedef struct nb_response {
    int status;
    const char *status_msg;
    nb_payload_t payload;
    const nb_metadata_t *metadata; /* may be NULL; callback scope only */
    bool owns_payload;
} nb_response_t;

typedef void (*nb_response_fn)(int status, const nb_response_t *resp, void *user);
typedef void (*nb_response_chunk_fn)(int status, const nb_chunk_t *chunk, void *user);
typedef void (*nb_stream_done_fn)(int status, void *user);

/* ---- RPC limits ---- */
#ifndef NB_DISCOVERY_PORT
#define NB_DISCOVERY_PORT 41953
#endif

#ifndef NB_DEFAULT_TIMEOUT_MS
#define NB_DEFAULT_TIMEOUT_MS 5000
#endif

#define NB_SERVICE_NAME_MAX 64
#define NB_METHOD_NAME_MAX 64
#define NB_MAX_BINDINGS 8
#define NB_MAX_ENDPOINTS 32

#define NB_MAX_IO_THREADS 8
#define NB_MAX_HANDLER_THREADS 1024
#define NB_MAX_SEND_LANES 64
#define NB_MAX_STREAM_RECV_STREAMS 64
#define NB_MAX_MSG_BYTES_LIMIT (64u * 1024u * 1024u)

#ifdef __cplusplus
}
#endif

#endif /* NB_RPC_TYPES_H */
