#ifndef NTK_DISCOVERY_H
#define NTK_DISCOVERY_H

/*
 * ntk_discovery - a small broadcast service-discovery library on top of ntk.
 *
 * It carries opaque key/value pairs between nodes on the same broadcast domain
 * (one hop). The library never interprets keys or values: the application
 * decides what a key means (for the RPC framework it is a service name) and how
 * to read its value (for the RPC framework it is a URI list).
 *
 * Wire format, data structures and design notes: docs/discovery-review.md.
 *
 * Runtime model: the caller owns a poller and drives the periodic work:
 *
 *   ntk_discovery_config_init(&cfg);
 *   d = ntk_discovery_create(poller, &cfg);
 *   ntk_discovery_set_identity(d, instance, "node-a");
 *   ntk_discovery_set_endpoints(d, eps, n);
 *   ntk_discovery_advertise(d, "Service", value, len, 0);
 *   ntk_timer_start(poller, 3000, 1, on_tick, d);   // tick + announce
 *
 * Receive, timer and callback code all run on the poller thread; the public
 * functions may be called from other threads.
 */

#include <ntk.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTK_DISC_VERSION_MAJOR 1u
#define NTK_DISC_VERSION_MINOR 0u

#define NTK_DISC_INSTANCE_SIZE 16u
#define NTK_DISC_NAME_MAX 64u
#define NTK_DISC_KEY_MAX 128u
#define NTK_DISC_VALUE_MAX 512u
#define NTK_DISC_URI_MAX 256u
#define NTK_DISC_MAX_ENDPOINTS 8u
#define NTK_DISC_DEFAULT_PORT 41953u

/* Return codes. */
enum {
    NTK_DISC_OK = 0,
    NTK_DISC_ERR_INVALID = -1,
    NTK_DISC_ERR_NOMEM = -2,
    NTK_DISC_ERR_IO = -3,
    NTK_DISC_ERR_TOOBIG = -4,
    NTK_DISC_ERR_STATE = -5,
    NTK_DISC_ERR_OVERLOAD = -6,
    NTK_DISC_ERR_NOTFOUND = -7
};

const char *ntk_disc_strerror(int error);

typedef struct ntk_discovery ntk_discovery_t;

/* One endpoint advertised for a node. kind is application-defined (the RPC
 * framework stores its transport kind); uri is an opaque string. */
typedef struct ntk_disc_endpoint {
    uint8_t kind;
    char uri[NTK_DISC_URI_MAX];
} ntk_disc_endpoint_t;

/* Snapshot of a peer. The strings are copied into the caller's struct, so a
 * callback may keep or re-use it after returning. */
typedef struct ntk_disc_peer_info {
    uint8_t instance[NTK_DISC_INSTANCE_SIZE];
    char name[NTK_DISC_NAME_MAX];
    char host[NTK_DISC_NAME_MAX]; /* source address of its last HELLO */
    uint16_t port;
    size_t endpoint_count;
    ntk_disc_endpoint_t endpoints[NTK_DISC_MAX_ENDPOINTS];
    uint64_t last_seen_ms;
} ntk_disc_peer_info_t;

/* Snapshot of one key/value entry. */
typedef struct ntk_disc_key_info {
    uint8_t instance[NTK_DISC_INSTANCE_SIZE]; /* owner */
    char key[NTK_DISC_KEY_MAX];
    uint8_t value[NTK_DISC_VALUE_MAX];
    size_t value_len;
    uint32_t ttl_ms;
} ntk_disc_key_info_t;

/*
 * Callbacks run on the poller thread with the library lock held (the mutex is
 * recursive, so snapshot calls from a callback are safe). They must not block.
 * `removed` is non-zero when the peer/key disappeared (BYE, TTL expiry, or a
 * withdrawn key). Key callbacks for a HELLO are emitted before its peer
 * callback, which is emitted once the whole peer state has been applied; a
 * callback that needs the full key set can call ntk_discovery_peer_keys().
 */
typedef struct ntk_discovery_callbacks {
    void (*on_peer)(void *user, const ntk_disc_peer_info_t *peer, int removed);
    void (*on_key)(void *user, const ntk_disc_key_info_t *key, int removed);
} ntk_discovery_callbacks_t;

typedef struct ntk_discovery_config {
    uint32_t struct_size;
    /* NULL or "0.0.0.0" binds every interface. */
    const char *bind_host;
    /* 0 = NTK_DISC_DEFAULT_PORT. */
    uint16_t port;
    /* Broadcast address; NULL = 255.255.255.255. */
    const char *broadcast;
    /* Suggested announce interval and jitter; the caller schedules the timer
     * and may use ntk_discovery_next_delay_ms(). */
    uint32_t hello_interval_ms; /* 0 = 3000 */
    uint32_t jitter_pct;        /* 0 = 20 */
    /* A peer is removed when no HELLO arrives for this long (0 = 9000). */
    uint32_t ttl_ms;
    uint32_t max_peers; /* 0 = 256 */
    uint32_t max_keys;  /* 0 = 4096 */
    ntk_discovery_callbacks_t callbacks;
    void *user;
} ntk_discovery_config_t;

void ntk_discovery_config_init(ntk_discovery_config_t *config);

/* Binds the socket and attaches it to poller. NULL on failure. */
ntk_discovery_t *ntk_discovery_create(ntk_poller *poller,
                                     const ntk_discovery_config_t *config);
void ntk_discovery_destroy(ntk_discovery_t *discovery);

/* Identity of this node. instance must be unique per running process; the RPC
 * framework stores its 32-bit node id in the first four bytes. */
int ntk_discovery_set_identity(ntk_discovery_t *discovery,
                               const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                               const char *name);
int ntk_discovery_set_endpoints(ntk_discovery_t *discovery,
                                const ntk_disc_endpoint_t *endpoints,
                                size_t count);

/* Local key/value advertisements. ttl_ms 0 = use the config TTL. */
int ntk_discovery_advertise(ntk_discovery_t *discovery,
                            const char *key,
                            const void *value,
                            size_t value_len,
                            uint32_t ttl_ms);
int ntk_discovery_withdraw(ntk_discovery_t *discovery, const char *key);
int ntk_discovery_clear_keys(ntk_discovery_t *discovery);

/* Send a HELLO (full state, fragmented when needed) or a BYE now. */
int ntk_discovery_announce(ntk_discovery_t *discovery);
int ntk_discovery_bye(ntk_discovery_t *discovery);

/* Expire peers and keys, and drop incomplete fragment sets. Call from a timer. */
void ntk_discovery_tick(ntk_discovery_t *discovery, uint64_t now_ms);
/* hello_interval_ms with jitter, for scheduling the next announce. */
uint32_t ntk_discovery_next_delay_ms(ntk_discovery_t *discovery);

/* Extra unicast announce targets (in addition to the broadcast address). */
int ntk_discovery_add_peer(ntk_discovery_t *discovery,
                           const char *host,
                           uint16_t port);

uint16_t ntk_discovery_port(const ntk_discovery_t *discovery);
size_t ntk_discovery_peer_count(const ntk_discovery_t *discovery);

/* Snapshots. Return the number written (0 when nothing matches). */
size_t ntk_discovery_peers(ntk_discovery_t *discovery,
                           ntk_disc_peer_info_t *out,
                           size_t max);
size_t ntk_discovery_peer_keys(ntk_discovery_t *discovery,
                               const uint8_t instance[NTK_DISC_INSTANCE_SIZE],
                               ntk_disc_key_info_t *out,
                               size_t max);
size_t ntk_discovery_lookup(ntk_discovery_t *discovery,
                            const char *key,
                            ntk_disc_key_info_t *out,
                            size_t max);

#ifdef __cplusplus
}
#endif

#endif /* NTK_DISCOVERY_H */
