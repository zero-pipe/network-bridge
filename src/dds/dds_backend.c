/* DDS spine: backend table. */
#include <nb/dds/backend.h>

#include <nb/base/error.h>

#include <stddef.h>

extern const nb_dds_backend_ops_t nb_dds_rtps_backend_ops;
#ifdef NB_ENABLE_DDS
extern const nb_dds_backend_ops_t nb_dds_cyclone_backend_ops;
#endif

#define NB_DDS_MAX_CUSTOM_BACKENDS 4

static const nb_dds_backend_ops_t *nb_g_backends[NB_DDS_MAX_CUSTOM_BACKENDS];
static size_t nb_g_backend_count;

const nb_dds_backend_ops_t *nb_dds_backend_find(nb_dds_backend_t backend)
{
    size_t i;

    switch (backend) {
    case NB_DDS_BACKEND_RTPS:
        return &nb_dds_rtps_backend_ops;
#ifdef NB_ENABLE_DDS
    case NB_DDS_BACKEND_CYCLONE:
        return &nb_dds_cyclone_backend_ops;
#endif
    default:
        break;
    }
    for (i = 0; i < nb_g_backend_count; i++) {
        if (nb_g_backends[i]->id == backend) {
            return nb_g_backends[i];
        }
    }
    return NULL;
}

int nb_dds_backend_register(const nb_dds_backend_ops_t *ops)
{
    if (!ops || ops->abi_version != NB_DDS_BACKEND_ABI_VERSION || !ops->participant_create ||
        !ops->participant_destroy || !ops->topic_create || !ops->writer_create ||
        !ops->reader_create) {
        return NB_ERR_INVALID;
    }
    if (nb_dds_backend_find(ops->id)) {
        return NB_ERR_ALREADY;
    }
    if (nb_g_backend_count >= NB_DDS_MAX_CUSTOM_BACKENDS) {
        return NB_ERR_NOMEM;
    }
    nb_g_backends[nb_g_backend_count++] = ops;
    return NB_OK;
}
