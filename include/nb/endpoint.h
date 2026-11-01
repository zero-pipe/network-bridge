#ifndef NB_ENDPOINT_H
#define NB_ENDPOINT_H

#include <nb/types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_ENDPOINT_URI_MAX 256
#define NB_ENDPOINT_PATH_MAX 128

/* ---- Data structure ---- */
/*
 * Transport-neutral endpoint. scheme selects a provider, host holds the
 * provider authority (IP/hostname, DDS domain, or SHM channel), port is the
 * provider service id, and uri is the canonical wire/storage form.
 *
 * Two fields are derived rather than independent:
 *   - transport is the built-in kind for udp/tcp, and NB_TRANSPORT_KIND_CUSTOM for
 *     provider schemes (shm, dds, ...). scheme is the source of truth.
 *   - uri is a cached snapshot refreshed by nb_endpoint_refresh() /
 *     nb_endpoint_from_parts(); callers must not assume it stays in sync after
 *     editing host/port by hand.
 *
 * All framework paths (route/registry/discovery/proxy/provider) use this type
 * so provider-specific addressing is never rebuilt from an ip:port pair.
 */
typedef struct nb_endpoint {
    char scheme[NB_SCHEME_MAX];
    char host[NB_HOST_MAX];
    char path[NB_ENDPOINT_PATH_MAX];
    uint16_t port;
    /* False for provider schemes whose authority has no port component (for
     * example dds://0). UDP/TCP endpoints always have a port. */
    bool has_port;
    nb_transport_kind_t transport;
    char uri[NB_ENDPOINT_URI_MAX];
} nb_endpoint_t;

/* ---- Scheme and transport mapping ---- */
/* Map a provider scheme to a built-in transport kind; provider schemes map to
 * NB_TRANSPORT_KIND_CUSTOM. */
nb_transport_kind_t nb_transport_kind_from_scheme(const char *scheme);

/* ---- Parse and format ---- */
/* Parse scheme://host[:port][/path]. The input is not retained. The port is
 * required for tcp/udp and optional for other schemes (for example dds://0,
 * where has_port stays false); a port of 0 is valid for tcp/udp and means
 * "assign by the system". */
int nb_endpoint_parse(const char *uri, nb_endpoint_t *out);

/* Build an endpoint from parts and fill its canonical URI. transport must
 * agree with scheme (NB_TRANSPORT_KIND_CUSTOM for provider schemes). */
int nb_endpoint_from_parts(nb_endpoint_t *out,
                           const char *scheme,
                           const char *host,
                           uint16_t port,
                           nb_transport_kind_t transport);

/* Format an endpoint URI into out (canonical form, ignores endpoint->uri). */
int nb_endpoint_format(const nb_endpoint_t *endpoint, char *out, size_t cap);

/* Refresh the cached endpoint->uri after fields were changed. */
int nb_endpoint_refresh(nb_endpoint_t *endpoint);

/* True for UDP/TCP, whose host is a network address discovery may rewrite. */
bool nb_endpoint_is_network(const nb_endpoint_t *endpoint);

/* Compare scheme, host, has_port, port, and path. */
bool nb_endpoint_equal(const nb_endpoint_t *a, const nb_endpoint_t *b);

#ifdef __cplusplus
}
#endif

#endif /* NB_ENDPOINT_H */
