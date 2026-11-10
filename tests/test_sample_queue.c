/*
 * DDS reader history: KEEP_LAST, state masks, instance filtering, borrowed
 * read vs owned take. Pure unit test of src/dds/sample_queue.c.
 */
#include "sample_queue.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            fails++;                                                                         \
        }                                                                                    \
    } while (0)

static int payload_is(const nb_dds_sample_t *sample, const char *text)
{
    size_t len = strlen(text);
    return sample->data.len == len && sample->data.data &&
           memcmp(sample->data.data, text, len) == 0;
}

static void push(nb_sample_queue_t *queue, const char *text, const nb_dds_sample_info_t *info)
{
    CHECK(nb_sample_queue_push(queue, text, strlen(text), 0, info) == NB_OK);
}

static void test_keep_last_and_read_take(void)
{
    nb_sample_queue_t *queue = nb_sample_queue_create(3);
    nb_dds_sample_filter_t read_filter = {NB_DDS_STATE_SAMPLE_READ, NULL};
    nb_dds_sample_filter_t not_read = {NB_DDS_STATE_SAMPLE_NOT_READ, NULL};
    nb_dds_sample_t out[8];
    nb_dds_sample_info_t info;
    size_t n;

    CHECK(queue != NULL);
    if (!queue) {
        return;
    }
    memset(&info, 0, sizeof(info));
    push(queue, "0", &info);
    push(queue, "1", &info);
    push(queue, "2", &info);
    CHECK(nb_sample_queue_size(queue) == 3);

    /* KEEP_LAST: the two oldest samples are dropped. */
    push(queue, "3", &info);
    push(queue, "4", &info);
    CHECK(nb_sample_queue_size(queue) == 3);
    CHECK(nb_sample_queue_dropped(queue) == 2);

    n = nb_sample_queue_read(queue, NULL, out, 8);
    CHECK(n == 3);
    CHECK(payload_is(&out[0], "2"));
    CHECK(payload_is(&out[1], "3"));
    CHECK(payload_is(&out[2], "4"));
    CHECK(out[0].info.state == NB_SAMPLE_STATE_VALID);
    CHECK(nb_sample_info_valid_data(&out[0].info));
    CHECK(out[0].owner == NULL); /* borrowed */

    /* read() marks samples as READ but keeps them. */
    CHECK(nb_sample_queue_read(queue, &not_read, out, 8) == 0);
    CHECK(nb_sample_queue_read(queue, &read_filter, out, 8) == 3);

    /* take() removes them and hands back owned buffers. */
    n = nb_sample_queue_take(queue, &read_filter, out, 8);
    CHECK(n == 3);
    CHECK(payload_is(&out[0], "2"));
    CHECK(out[0].owner != NULL);
    CHECK(nb_sample_queue_size(queue) == 0);
    nb_dds_sample_release(out, n);

    /* Depth changes are honoured on the next push. */
    nb_sample_queue_set_depth(queue, 2);
    push(queue, "a", &info);
    push(queue, "b", &info);
    push(queue, "c", &info);
    CHECK(nb_sample_queue_size(queue) == 2);
    CHECK(nb_sample_queue_take(queue, NULL, out, 8) == 2);
    CHECK(payload_is(&out[0], "b"));
    CHECK(payload_is(&out[1], "c"));
    nb_dds_sample_release(out, 2);

    nb_sample_queue_destroy(queue);
}

static void test_state_masks_and_instances(void)
{
    nb_sample_queue_t *queue = nb_sample_queue_create(8);
    nb_dds_sample_filter_t disposed = {NB_DDS_STATE_INSTANCE_DISPOSED, NULL};
    nb_dds_sample_filter_t no_writers = {NB_DDS_STATE_INSTANCE_NO_WRITERS, NULL};
    nb_dds_sample_filter_t new_not_read = {NB_DDS_STATE_VIEW_NEW | NB_DDS_STATE_SAMPLE_NOT_READ,
                                           NULL};
    nb_dds_sample_filter_t instance_a = {NB_DDS_STATE_ANY, NULL};
    uint8_t handle_a[NB_INSTANCE_HANDLE_SIZE];
    uint8_t handle_b[NB_INSTANCE_HANDLE_SIZE];
    nb_dds_sample_info_t info;
    nb_dds_sample_t out[8];
    size_t n;

    CHECK(queue != NULL);
    if (!queue) {
        return;
    }
    memset(handle_a, 0x11, sizeof(handle_a));
    memset(handle_b, 0x22, sizeof(handle_b));
    instance_a.instance = handle_a;

    memset(&info, 0, sizeof(info));
    info.has_instance_handle = true;
    memcpy(info.instance_handle, handle_a, sizeof(handle_a));
    info.view_state = NB_VIEW_STATE_NEW;
    info.instance_state = NB_INSTANCE_STATE_ALIVE;
    push(queue, "A", &info);

    memset(&info, 0, sizeof(info));
    info.state = NB_SAMPLE_STATE_DISPOSED;
    info.has_instance_handle = true;
    memcpy(info.instance_handle, handle_b, sizeof(handle_b));
    info.view_state = NB_VIEW_STATE_NEW;
    info.instance_state = NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED;
    push(queue, "B", &info);

    memset(&info, 0, sizeof(info));
    info.state = NB_SAMPLE_STATE_NO_WRITERS;
    info.has_instance_handle = true;
    memcpy(info.instance_handle, handle_b, sizeof(handle_b));
    info.view_state = NB_VIEW_STATE_NOT_NEW;
    info.instance_state = NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS;
    push(queue, "C", &info);

    n = nb_sample_queue_read(queue, &new_not_read, out, 8);
    CHECK(n == 2);

    n = nb_sample_queue_take(queue, &disposed, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "B"));
    CHECK(!nb_sample_info_valid_data(&out[0].info));
    nb_dds_sample_release(out, n);

    n = nb_sample_queue_take(queue, &no_writers, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "C"));
    CHECK(out[0].info.instance_state == NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS);
    nb_dds_sample_release(out, n);

    n = nb_sample_queue_take(queue, &instance_a, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "A"));
    nb_dds_sample_release(out, n);

    CHECK(nb_sample_queue_take(queue, NULL, out, 8) == 0);
    nb_sample_queue_destroy(queue);
}

int main(void)
{
    test_keep_last_and_read_take();
    test_state_masks_and_instances();

    if (fails) {
        printf("test_sample_queue: %d check(s) failed\n", fails);
        return 1;
    }
    printf("test_sample_queue: ok\n");
    return 0;
}
