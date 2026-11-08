/*
 * Reader history (the queue behind a pub/sub history reader): KEEP_LAST,
 * state masks, instance filtering, borrowed read vs owned take. This is a pure
 * unit test of src/core/sample_queue.c; the end-to-end reader path is covered
 * by test_pubsub.c.
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

static void init_msg(nb_message_t *msg, const char *text)
{
    memset(msg, 0, sizeof(*msg));
    msg->kind = NB_MSG_KIND_EVENT;
    msg->payload.data = text;
    msg->payload.len = strlen(text);
}

static int payload_is(const nb_sample_t *sample, const char *text)
{
    size_t len = strlen(text);
    return sample->data.len == len && sample->data.data &&
           memcmp(sample->data.data, text, len) == 0;
}

static void test_keep_last_and_read_take(void)
{
    nb_sample_queue_t *queue = nb_sample_queue_create(3);
    nb_sample_filter_t read_filter = {NB_STATE_SAMPLE_READ, NULL};
    nb_sample_filter_t not_read = {NB_STATE_SAMPLE_NOT_READ, NULL};
    nb_message_t msg;
    nb_sample_t out[8];
    size_t n;

    CHECK(queue != NULL);
    if (!queue) {
        return;
    }
    init_msg(&msg, "0");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "1");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "2");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    CHECK(nb_sample_queue_size(queue) == 3);

    /* KEEP_LAST: the two oldest samples are dropped. */
    init_msg(&msg, "3");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "4");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    CHECK(nb_sample_queue_size(queue) == 3);
    CHECK(nb_sample_queue_dropped(queue) == 2);

    n = nb_sample_queue_read(queue, NULL, out, 8);
    CHECK(n == 3);
    CHECK(payload_is(&out[0], "2"));
    CHECK(payload_is(&out[1], "3"));
    CHECK(payload_is(&out[2], "4"));
    CHECK(out[0].info.state == NB_SAMPLE_STATE_VALID);
    CHECK(nb_sample_info_valid_data(&out[0].info));

    /* read() marks samples as READ but keeps them. */
    CHECK(nb_sample_queue_read(queue, &not_read, out, 8) == 0);
    CHECK(nb_sample_queue_read(queue, &read_filter, out, 8) == 3);

    /* take() removes them and hands back owned copies. */
    n = nb_sample_queue_take(queue, &read_filter, out, 8);
    CHECK(n == 3);
    CHECK(payload_is(&out[0], "2"));
    CHECK(nb_sample_queue_size(queue) == 0);
    for (n = 0; n < 3; n++) {
        nb_sample_release(&out[n]);
    }

    /* Depth changes are honoured on the next push. */
    nb_sample_queue_set_depth(queue, 2);
    init_msg(&msg, "a");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "b");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    init_msg(&msg, "c");
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);
    CHECK(nb_sample_queue_size(queue) == 2);
    CHECK(nb_sample_queue_take(queue, NULL, out, 8) == 2);
    CHECK(payload_is(&out[0], "b"));
    CHECK(payload_is(&out[1], "c"));
    nb_sample_release(&out[0]);
    nb_sample_release(&out[1]);

    nb_sample_queue_destroy(queue);
}

static void test_state_masks_and_instances(void)
{
    nb_sample_queue_t *queue = nb_sample_queue_create(8);
    nb_sample_filter_t disposed = {NB_STATE_INSTANCE_DISPOSED, NULL};
    nb_sample_filter_t no_writers = {NB_STATE_INSTANCE_NO_WRITERS, NULL};
    nb_sample_filter_t new_not_read = {NB_STATE_VIEW_NEW | NB_STATE_SAMPLE_NOT_READ, NULL};
    nb_sample_filter_t instance_a = {NB_STATE_ANY, NULL};
    uint8_t handle_a[NB_INSTANCE_HANDLE_SIZE];
    uint8_t handle_b[NB_INSTANCE_HANDLE_SIZE];
    nb_message_t msg;
    nb_sample_t out[8];
    size_t n;

    CHECK(queue != NULL);
    if (!queue) {
        return;
    }
    memset(handle_a, 0x11, sizeof(handle_a));
    memset(handle_b, 0x22, sizeof(handle_b));
    instance_a.instance = handle_a;

    init_msg(&msg, "A");
    msg.sample.has_instance_handle = true;
    memcpy(msg.sample.instance_handle, handle_a, sizeof(handle_a));
    msg.sample.view_state = NB_VIEW_STATE_NEW;
    msg.sample.instance_state = NB_INSTANCE_STATE_ALIVE;
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);

    init_msg(&msg, "B");
    msg.sample.state = NB_SAMPLE_STATE_DISPOSED;
    msg.sample.has_instance_handle = true;
    memcpy(msg.sample.instance_handle, handle_b, sizeof(handle_b));
    msg.sample.view_state = NB_VIEW_STATE_NEW;
    msg.sample.instance_state = NB_INSTANCE_STATE_NOT_ALIVE_DISPOSED;
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);

    init_msg(&msg, "C");
    msg.sample.state = NB_SAMPLE_STATE_NO_WRITERS;
    msg.sample.has_instance_handle = true;
    memcpy(msg.sample.instance_handle, handle_b, sizeof(handle_b));
    msg.sample.view_state = NB_VIEW_STATE_NOT_NEW;
    msg.sample.instance_state = NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS;
    CHECK(nb_sample_queue_push(queue, &msg) == NB_OK);

    n = nb_sample_queue_read(queue, &new_not_read, out, 8);
    CHECK(n == 2);

    n = nb_sample_queue_take(queue, &disposed, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "B"));
    CHECK(!nb_sample_info_valid_data(&out[0].info));
    nb_sample_release(&out[0]);

    n = nb_sample_queue_take(queue, &no_writers, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "C"));
    CHECK(out[0].info.instance_state == NB_INSTANCE_STATE_NOT_ALIVE_NO_WRITERS);
    nb_sample_release(&out[0]);

    n = nb_sample_queue_take(queue, &instance_a, out, 8);
    CHECK(n == 1);
    CHECK(payload_is(&out[0], "A"));
    nb_sample_release(&out[0]);

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
