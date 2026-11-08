#include <nb/types.h>

#include <nb/buf.h>

void nb_owned_payload_init(nb_owned_payload_t *payload,
                          nb_buf_t *buf,
                          size_t offset,
                          size_t len,
                          uint8_t encoding)
{
    if (!payload) {
        return;
    }
    payload->buf = buf;
    payload->offset = offset;
    payload->len = len;
    payload->encoding = encoding;
}

nb_buf_t *nb_owned_payload_take(nb_owned_payload_t *payload)
{
    nb_buf_t *buf;

    if (!payload) {
        return NULL;
    }
    buf = payload->buf;
    payload->buf = NULL;
    payload->offset = 0;
    payload->len = 0;
    return buf;
}

void nb_owned_payload_release(nb_owned_payload_t *payload)
{
    if (!payload || !payload->buf) {
        return;
    }
    nb_buf_release(payload->buf);
    payload->buf = NULL;
    payload->offset = 0;
    payload->len = 0;
}

nb_payload_t nb_owned_payload_view(const nb_owned_payload_t *payload)
{
    nb_payload_t view;
    size_t size;

    view.data = NULL;
    view.len = 0;
    view.encoding = 0;
    if (!payload || !payload->buf) {
        return view;
    }
    size = nb_buf_size(payload->buf);
    if (payload->offset > size || payload->len > size - payload->offset) {
        return view;
    }
    view.data = nb_buf_const_data(payload->buf) + payload->offset;
    view.len = payload->len;
    view.encoding = payload->encoding;
    return view;
}
