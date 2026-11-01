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
        const nb_transport_provider_ops_t *ops;

        if (!transport || !transport->provider) {
            continue;
        }
        ops = transport->provider->ops;
        if (!ops || !ops->discovery_read) {
            continue;
        }
        /*
         * External providers compiled against an older ops table report their
         * real size; do not read past the end of what they exported.
         */
        if (ops->struct_size != 0 &&
            ops->struct_size <
                offsetof(nb_transport_provider_ops_t, discovery_read) +
                    sizeof(ops->discovery_read)) {
            continue;
        }
        total += ops->discovery_read(
            transport->provider_instance, (int)topic, out + total, max - total);
    }
    return total;
}
