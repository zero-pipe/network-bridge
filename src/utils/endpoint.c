#include <nb/endpoint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nb_copy_part(char *out, size_t cap, const char *begin, size_t len)
{
    if (!out || cap == 0 || len == 0 || len >= cap) {
        return NB_ERR_INVALID;
    }
    memcpy(out, begin, len);
    out[len] = '\0';
    return NB_OK;
}

static int nb_endpoint_format_raw(const nb_endpoint_t *endpoint, char *out, size_t cap)
{
    int n;
    bool with_port;

    if (!endpoint || !out || cap == 0 || !endpoint->scheme[0] || !endpoint->host[0]) {
        return NB_ERR_INVALID;
    }
    with_port = endpoint->has_port || endpoint->transport == NB_TRANSPORT_KIND_UDP ||
                endpoint->transport == NB_TRANSPORT_KIND_TCP || strcmp(endpoint->scheme, "udp") == 0 ||
                strcmp(endpoint->scheme, "tcp") == 0;
    if (strchr(endpoint->host, ':')) {
        if (!with_port) {
            n = snprintf(out, cap, "%s://[%s]", endpoint->scheme, endpoint->host);
        } else {
            n = snprintf(
                out, cap, "%s://[%s]:%u", endpoint->scheme, endpoint->host, (unsigned)endpoint->port);
        }
    } else if (!with_port) {
        n = snprintf(out, cap, "%s://%s", endpoint->scheme, endpoint->host);
    } else {
        n = snprintf(
            out, cap, "%s://%s:%u", endpoint->scheme, endpoint->host, (unsigned)endpoint->port);
    }
    if (n < 0 || (size_t)n >= cap) {
        return NB_ERR_TOOBIG;
    }
    if (endpoint->path[0]) {
        const char *separator = endpoint->path[0] == '/' ? "" : "/";
        int m = snprintf(out + n, cap - (size_t)n, "%s%s", separator, endpoint->path);
        if (m < 0 || (size_t)m >= cap - (size_t)n) {
            return NB_ERR_TOOBIG;
        }
    }
    return NB_OK;
}

/* ---- Scheme 与传输映射 ---- */

nb_transport_kind_t nb_transport_kind_from_scheme(const char *scheme)
{
    if (!scheme) {
        return (nb_transport_kind_t)-1;
    }
    if (strcmp(scheme, "tcp") == 0 || strcmp(scheme, "TCP") == 0) {
        return NB_TRANSPORT_KIND_TCP;
    }
    if (strcmp(scheme, "udp") == 0 || strcmp(scheme, "UDP") == 0) {
        return NB_TRANSPORT_KIND_UDP;
    }
    return NB_TRANSPORT_KIND_CUSTOM;
}

/* ---- 解析与格式化 ---- */

int nb_endpoint_parse(const char *uri, nb_endpoint_t *out)
{
    const char *scheme_end;
    const char *authority;
    const char *auth_end;
    const char *path_sep;
    const char *host_begin;
    const char *host_end;
    const char *port_begin = NULL;
    const char *port_stop = NULL;
    nb_transport_kind_t kind;
    int network;

    if (!uri || !out) {
        return NB_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    scheme_end = strstr(uri, "://");
    if (!scheme_end || scheme_end == uri) {
        return NB_ERR_INVALID;
    }
    if (nb_copy_part(out->scheme, sizeof(out->scheme), uri, (size_t)(scheme_end - uri)) != NB_OK) {
        return NB_ERR_INVALID;
    }
    kind = nb_transport_kind_from_scheme(out->scheme);
    network = kind == NB_TRANSPORT_KIND_UDP || kind == NB_TRANSPORT_KIND_TCP;
    authority = scheme_end + 3;
    if (!*authority || *authority == '/') {
        return NB_ERR_INVALID;
    }
    path_sep = strchr(authority, '/');
    auth_end = path_sep ? path_sep : authority + strlen(authority);

    if (*authority == '[') {
        const char *close = (const char *)memchr(authority, ']', (size_t)(auth_end - authority));
        if (!close || close == authority + 1) {
            return NB_ERR_INVALID;
        }
        host_begin = authority + 1;
        host_end = close;
        if (close + 1 < auth_end) {
            if (close[1] != ':') {
                return NB_ERR_INVALID;
            }
            port_begin = close + 2;
            port_stop = auth_end;
        }
    } else {
        const char *colon = NULL;
        const char *p;
        for (p = authority; p < auth_end; p++) {
            if (*p == ':') {
                colon = p;
            }
        }
        host_begin = authority;
        host_end = colon ? colon : auth_end;
        if (colon) {
            port_begin = colon + 1;
            port_stop = auth_end;
        }
    }
    if (host_end == host_begin ||
        nb_copy_part(out->host, sizeof(out->host), host_begin, (size_t)(host_end - host_begin)) !=
            NB_OK) {
        return NB_ERR_INVALID;
    }
    if (port_begin) {
        char port_text[8];
        char *port_end;
        unsigned long port;
        size_t digits = (size_t)(port_stop - port_begin);

        if (digits == 0 || digits >= sizeof(port_text)) {
            return NB_ERR_INVALID;
        }
        memcpy(port_text, port_begin, digits);
        port_text[digits] = '\0';
        port = strtoul(port_text, &port_end, 10);
        if (*port_end != '\0' || port > 65535) {
            return NB_ERR_INVALID;
        }
        out->port = (uint16_t)port;
        out->has_port = true;
    } else {
        /* Only provider schemes may omit the port; tcp/udp need one. */
        if (network) {
            return NB_ERR_INVALID;
        }
        out->has_port = false;
    }
    out->transport = kind;
    if (path_sep) {
        size_t path_len = strlen(path_sep);

        if (path_len >= sizeof(out->path)) {
            return NB_ERR_INVALID;
        }
        memcpy(out->path, path_sep, path_len + 1u);
    }
    if (nb_endpoint_refresh(out) != NB_OK) {
        memset(out, 0, sizeof(*out));
        return NB_ERR_INVALID;
    }
    return NB_OK;
}

int nb_endpoint_format(const nb_endpoint_t *endpoint, char *out, size_t cap)
{
    return nb_endpoint_format_raw(endpoint, out, cap);
}

int nb_endpoint_refresh(nb_endpoint_t *endpoint)
{
    if (!endpoint) {
        return NB_ERR_INVALID;
    }
    return nb_endpoint_format_raw(endpoint, endpoint->uri, sizeof(endpoint->uri));
}

int nb_endpoint_from_parts(nb_endpoint_t *out,
                           const char *scheme,
                           const char *host,
                           uint16_t port,
                           nb_transport_kind_t transport)
{
    if (!out || !scheme || !scheme[0] || !host || !host[0]) {
        return NB_ERR_INVALID;
    }
    /* scheme is the source of truth: a caller may not label a dds/shm URI as
     * UDP/TCP (or vice versa), because routing selects transports by scheme. */
    if (nb_transport_kind_from_scheme(scheme) != transport) {
        return NB_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (nb_copy_part(out->scheme, sizeof(out->scheme), scheme, strlen(scheme)) != NB_OK ||
        nb_copy_part(out->host, sizeof(out->host), host, strlen(host)) != NB_OK) {
        memset(out, 0, sizeof(*out));
        return NB_ERR_INVALID;
    }
    out->port = port;
    out->transport = transport;
    out->has_port = (port != 0) || transport == NB_TRANSPORT_KIND_UDP || transport == NB_TRANSPORT_KIND_TCP;
    return nb_endpoint_refresh(out);
}

bool nb_endpoint_is_network(const nb_endpoint_t *endpoint)
{
    if (!endpoint) {
        return false;
    }
    return endpoint->transport == NB_TRANSPORT_KIND_UDP || endpoint->transport == NB_TRANSPORT_KIND_TCP ||
           strcmp(endpoint->scheme, "udp") == 0 || strcmp(endpoint->scheme, "tcp") == 0;
}

bool nb_endpoint_equal(const nb_endpoint_t *a, const nb_endpoint_t *b)
{
    if (!a || !b) {
        return false;
    }
    return a->port == b->port && a->has_port == b->has_port && strcmp(a->scheme, b->scheme) == 0 &&
           strcmp(a->host, b->host) == 0 && strcmp(a->path, b->path) == 0;
}
