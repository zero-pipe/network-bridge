#ifndef NB_DDS_READER_H
#define NB_DDS_READER_H

/*
 * DDS spine: reader (subscription) and wait sets.
 *
 * A reader is either a callback reader (on_data non-NULL) or a history reader
 * (on_data NULL): the latter keeps a KEEP_LAST queue of `qos.history.depth`
 * samples that read()/take() drain. Callbacks run on the backend thread and
 * the sample is borrowed for the duration of the call.
 */

#include <nb/dds/topic.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nb_dds_reader_data_fn)(void *user, const nb_dds_sample_t *sample);

int nb_dds_reader_create(nb_dds_topic_t *topic,
                         const nb_dds_qos_t *qos,
                         nb_dds_reader_data_fn on_data,
                         void *user,
                         nb_dds_reader_t **out);
void nb_dds_reader_destroy(nb_dds_reader_t *reader);

/* History readers only. read() returns borrowed views valid until the sample is
 * taken/evicted; take() transfers ownership and samples must be released with
 * nb_dds_sample_release(). */
size_t nb_dds_reader_read(nb_dds_reader_t *reader,
                          const nb_dds_sample_filter_t *filter,
                          nb_dds_sample_t *out,
                          size_t max);
size_t nb_dds_reader_take(nb_dds_reader_t *reader,
                          const nb_dds_sample_filter_t *filter,
                          nb_dds_sample_t *out,
                          size_t max);
size_t nb_dds_reader_size(const nb_dds_reader_t *reader);
uint64_t nb_dds_reader_dropped(const nb_dds_reader_t *reader);

/* ---- Wait sets -------------------------------------------------------- */
#define NB_DDS_WAITSET_INFINITE 0xFFFFFFFFu

int nb_dds_waitset_create(nb_dds_participant_t *participant, nb_dds_waitset_t **out);
void nb_dds_waitset_destroy(nb_dds_waitset_t *waitset);
/* Only history readers can be attached; a callback reader returns
 * NB_ERR_INVALID. */
int nb_dds_waitset_attach_reader(nb_dds_waitset_t *waitset, nb_dds_reader_t *reader);
int nb_dds_waitset_detach_reader(nb_dds_waitset_t *waitset, nb_dds_reader_t *reader);
size_t nb_dds_waitset_ready(nb_dds_waitset_t *waitset,
                            const nb_dds_sample_filter_t *filter);
int nb_dds_waitset_wait(nb_dds_waitset_t *waitset,
                        const nb_dds_sample_filter_t *filter,
                        uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_READER_H */
