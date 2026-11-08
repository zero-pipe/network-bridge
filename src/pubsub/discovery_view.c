#include <nb/discovery_view.h>

#include "node_internal.h"

#include <stddef.h>

size_t nb_node_discovery_read(nb_node_t *node,
                              nb_builtin_topic_t topic,
                              nb_discovery_sample_t *out,
                              size_t max)
{
    size_t total = 0;
    size_t i;

    if (!node || !out || max == 0 || (int)topic < 0 || topic > NB_BUILTIN_TOPIC_TOPIC) {
        return 0;
    }
    for (i = 0; i < node->transport_count && total < max; i++) {
        nb_transport_t *transport = node->transports[i];
        const nb_ps_backend_ops_t *ps;

        if (!transport || !transport->provider) {
            continue;
        }
        ps = transport->provider->ops->ps;
        if (!ps || !ps->discovery_read) {
            continue;
        }
        total += ps->discovery_read(
            transport->provider_instance, (int)topic, out + total, max - total);
    }
    return total;
}
