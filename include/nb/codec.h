#ifndef NB_CODEC_H
#define NB_CODEC_H

/*
 * Payload codec extension point (reserved for the application layer).
 *
 * The framework does not interpret payloads. An application may register an
 * nb_codec_t that converts business objects to and from byte streams keyed by
 * the encoding tag. The protocol carries only the encoding label; the
 * serialization format is up to the application, so JSON / IDL / protobuf and
 * the like can be added later.
 *
 * Typical use:
 *   - before send: codec->encode(user, obj, &bytes, &len) produces a payload;
 *   - after receive: codec->decode(user, payload.data, payload.len, &obj)
 *     restores the object;
 *   - release: codec->release(user, obj).
 */

#include <nb/types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Codec descriptor ---- */
typedef struct nb_codec {
    const char *name;
    uint8_t encoding; /* matches nb_payload_t.encoding */

    /*
     * Business object -> byte stream. The implementation allocates *out_data
     * and the caller frees it with release. When passed to
     * nb_proxy_call_async / nb_stub_call_async / nb_proxy_call_event, the
     * framework copies the bytes before returning, so release may run
     * immediately after the call returns.
     */
    int (*encode)(void *user, const void *obj, void **out_data, size_t *out_len);
    /* Byte stream -> business object. The implementation allocates *out_obj and
     * the caller frees it with release. */
    int (*decode)(void *user, const void *data, size_t len, void **out_obj);
    /* Free an object produced by encode/decode. */
    void (*release)(void *user, void *obj);

    void *user;
} nb_codec_t;

/* ---- Node registration ---- */
/* Register/look up a codec on a node. encoding must be unique; lookup returns
 * NULL when absent. */
int nb_node_register_codec(nb_node_t *node, const nb_codec_t *codec);
const nb_codec_t *nb_node_find_codec(const nb_node_t *node, uint8_t encoding);

#ifdef __cplusplus
}
#endif

#endif /* NB_CODEC_H */
