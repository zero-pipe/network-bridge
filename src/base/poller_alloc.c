#include <nb/internal/poller_alloc.h>

#include <nb/base/buf.h>

typedef struct nb_poller_hdr {
    nb_buf_t *buf;
} nb_poller_hdr_t;

void *nb_poller_alloc(size_t size)
{
    nb_buf_t *buf;
    nb_poller_hdr_t *h;
    size_t need;

    if (size == 0 || size > (size_t)-1 - sizeof(*h)) {
        return NULL;
    }
    need = sizeof(*h) + size;
    buf = nb_buf_alloc_local(need);
    if (!buf) {
        buf = nb_buf_alloc(need);
    }
    if (!buf) {
        return NULL;
    }
    h = (nb_poller_hdr_t *)nb_buf_data(buf);
    h->buf = buf;
    return (void *)(h + 1);
}

void nb_poller_free(void *ptr)
{
    nb_poller_hdr_t *h;

    if (!ptr) {
        return;
    }
    h = ((nb_poller_hdr_t *)ptr) - 1;
    nb_buf_release(h->buf);
}
