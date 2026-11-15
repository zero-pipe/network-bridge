#include "poller_internal.h"
#include "zn/util/buf.h"

/* defined in util/buf.c — bind lockless pool owner for cross-thread release */
void zn_buf_pool_set_owner_poller(zn_buf_pool *pool, zn_poller *owner);

void zn_poller_attach_buf_pool(zn_poller *poller, zn_buf_pool *pool)
{
    if (!poller) {
        return;
    }
    if (poller->buf_pool && poller->buf_pool != pool) {
        zn_buf_pool_set_owner_poller(poller->buf_pool, NULL);
    }
    poller->buf_pool = pool;
    if (pool) {
        zn_buf_pool_set_owner_poller(pool, poller);
    }
}

zn_buf_pool *zn_poller_buf_pool(zn_poller *poller)
{
    return poller ? poller->buf_pool : NULL;
}
