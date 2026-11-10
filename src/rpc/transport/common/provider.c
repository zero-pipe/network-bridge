#include "transport_internal.h"

#include <stddef.h>
#include <string.h>

#define PROVIDER_MAX 32

static const nb_transport_provider_t *nb_g_providers[PROVIDER_MAX];
static size_t nb_g_provider_count;

/* ---- Provider 注册 ---- */

int nb_transport_provider_register(const nb_transport_provider_t *provider)
{
    size_t i;
    const nb_transport_provider_ops_t *ops;

    if (!provider || !provider->scheme || !provider->scheme[0] || !provider->ops) {
        return NB_ERR_INVALID;
    }
    ops = provider->ops;
    if (!ops->create || !ops->start || !ops->stop || !ops->destroy) {
        return NB_ERR_INVALID;
    }
    /* A transport provider must expose the complete RPC interface. */
    if (!ops->rpc || !ops->rpc->send || !ops->rpc->reply) {
        return NB_ERR_INVALID;
    }
    /* The ops layout changes with the spine split; only a provider built against
     * this header can be interpreted. */
    if (ops->abi_version != NB_TRANSPORT_PROVIDER_ABI_VERSION) {
        return NB_ERR_NOT_SUPPORTED;
    }
    if (ops->struct_size != 0u && ops->struct_size < sizeof(*ops)) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < nb_g_provider_count; i++) {
        if (strcmp(nb_g_providers[i]->scheme, provider->scheme) == 0) {
            return NB_ERR_STATE;
        }
    }
    if (nb_g_provider_count >= PROVIDER_MAX) {
        return NB_ERR_NOMEM;
    }
    nb_g_providers[nb_g_provider_count++] = provider;
    return NB_OK;
}

int nb_transport_provider_unregister(const char *scheme)
{
    size_t i;

    if (!scheme || !scheme[0]) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < nb_g_provider_count; i++) {
        if (strcmp(nb_g_providers[i]->scheme, scheme) == 0) {
            size_t j;
            for (j = i; j + 1 < nb_g_provider_count; j++) {
                nb_g_providers[j] = nb_g_providers[j + 1];
            }
            nb_g_provider_count--;
            return NB_OK;
        }
    }
    return NB_ERR_NOTFOUND;
}

const nb_transport_provider_t *nb_transport_provider_find(const char *scheme)
{
    size_t i;
    if (!scheme) {
        return NULL;
    }
    for (i = 0; i < nb_g_provider_count; i++) {
        if (strcmp(nb_g_providers[i]->scheme, scheme) == 0) {
            return nb_g_providers[i];
        }
    }
    return NULL;
}
