#ifndef NB_DDS_BACKEND_H
#define NB_DDS_BACKEND_H

/*
 * DDS backend SPI (internal but public for custom backends).
 *
 * The DDS spine owns the object graph (participant/topic/writer/reader),
 * reader histories and wait sets. A backend only maps topic/writer/reader
 * entities onto one DDS implementation and delivers samples through the
 * callback below. History and wait-set semantics therefore stay identical
 * across the rtps and cyclone backends.
 */

#include <nb/dds/participant.h>
#include <nb/dds/qos.h>
#include <nb/dds/sample.h>
#include <nb/dds/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DDS_BACKEND_ABI_VERSION 1u

/* data is valid only for the duration of the callback. */
typedef void (*nb_dds_backend_on_sample_fn)(void *user,
                                            const void *data,
                                            size_t len,
                                            uint32_t encoding,
                                            const nb_dds_sample_info_t *info);

typedef struct nb_dds_backend_ops {
    uint32_t abi_version; /* NB_DDS_BACKEND_ABI_VERSION */
    uint32_t struct_size;
    nb_dds_backend_t id;
    const char *name;

    int (*participant_create)(const nb_dds_participant_options_t *options, void **out);
    void (*participant_destroy)(void *participant);
    int (*participant_start)(void *participant);
    void (*participant_stop)(void *participant);

    int (*topic_create)(void *participant,
                        const char *wire_name,
                        const char *type_name,
                        const nb_dds_qos_t *qos,
                        void **out);
    void (*topic_destroy)(void *participant, void *topic);

    int (*writer_create)(void *participant,
                         void *topic,
                         const nb_dds_qos_t *qos,
                         void **out);
    int (*writer_write)(void *writer, const void *data, size_t len);
    void (*writer_destroy)(void *participant, void *writer);

    int (*reader_create)(void *participant,
                         void *topic,
                         const nb_dds_qos_t *qos,
                         nb_dds_backend_on_sample_fn on_sample,
                         void *user,
                         void **out);
    void (*reader_destroy)(void *participant, void *reader);

    int (*set_status_callback)(void *participant,
                               nb_dds_status_callback_t callback,
                               void *user);
    size_t (*discovery_read)(void *participant,
                             nb_dds_builtin_topic_t topic,
                             nb_dds_discovery_sample_t *out,
                             size_t max);
    void (*metrics_read)(const void *participant, nb_dds_metrics_t *out);
} nb_dds_backend_ops_t;

/* Returns NULL when the backend is not compiled in (e.g. cyclone with
 * NB_ENABLE_DDS=OFF). */
const nb_dds_backend_ops_t *nb_dds_backend_find(nb_dds_backend_t backend);

/*
 * Register a custom backend. Backends are matched by id; registering an id
 * that is already present returns NB_ERR_ALREADY. The descriptor must have
 * static lifetime. Optional; the two built-in backends are always present.
 */
int nb_dds_backend_register(const nb_dds_backend_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_BACKEND_H */
