/* 统一消息视图：请求/响应共用的 payload 载体。 */
#include <nb/message_view.h>

#include <stdlib.h>
#include <string.h>

/* ---- 生命周期 ---- */

void nb_message_view_init(nb_message_view_t *view)
{
    if (!view) {
        return;
    }
    view->data = NULL;
    view->len = 0;
    view->encoding = 0;
    view->owner = NULL;
}

void nb_message_view_release(nb_message_view_t *view)
{
    if (!view) {
        return;
    }
    nb_buf_release(view->owner);
    view->owner = NULL;
    view->data = NULL;
    view->len = 0;
}

/* ---- 写入 ---- */

int nb_message_view_set_borrowed(nb_message_view_t *view, const void *data, size_t len)
{
    if (!view || (len > 0 && !data)) {
        return NB_ERR_INVALID;
    }
    nb_buf_release(view->owner);
    view->owner = NULL;
    view->data = data;
    view->len = len;
    return NB_OK;
}

int nb_message_view_set_data(nb_message_view_t *view, const void *data, size_t len)
{
    nb_buf_t *copy = NULL;
    if (!view || (len > 0 && !data)) {
        return NB_ERR_INVALID;
    }
    if (len > 0) {
        copy = nb_buf_alloc(len);
        if (!copy) {
            return NB_ERR_NOMEM;
        }
        memcpy(nb_buf_data(copy), data, len);
    }
    nb_buf_release(view->owner);
    view->owner = copy;
    view->data = copy ? nb_buf_const_data(copy) : NULL;
    view->len = len;
    return NB_OK;
}

int nb_message_view_set_string(nb_message_view_t *view, const char *text)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    return nb_message_view_set_data(view, text, strlen(text));
}

int nb_message_view_set_payload(nb_message_view_t *view, const nb_payload_t *payload)
{
    int rc;
    if (!view) {
        return NB_ERR_INVALID;
    }
    if (!payload) {
        nb_buf_release(view->owner);
        view->owner = NULL;
        view->data = NULL;
        view->len = 0;
        view->encoding = 0;
        return NB_OK;
    }
    rc = nb_message_view_set_data(view, payload->data, payload->len);
    if (rc == NB_OK) {
        view->encoding = payload->encoding;
    }
    return rc;
}

/* ---- 导出 ---- */

void nb_message_view_to_payload(const nb_message_view_t *view, nb_payload_t *out)
{
    if (!out) {
        return;
    }
    if (!view) {
        out->data = NULL;
        out->len = 0;
        out->encoding = 0;
        return;
    }
    out->data = view->data;
    out->len = view->len;
    out->encoding = view->encoding;
}
