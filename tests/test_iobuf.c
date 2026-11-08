#include <nb/internal/iobuf.h>

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void test_copy_peek_consume(void)
{
    nb_iobuf_t b;
    const char a[] = "hello";
    const char c[] = " world";
    char out[32];
    nb_iov_t iov[4];

    nb_iobuf_init(&b);
    CHECK(nb_iobuf_append_copy(&b, a, sizeof(a) - 1) == 0);
    CHECK(nb_iobuf_append_copy(&b, c, sizeof(c) - 1) == 0);
    CHECK(nb_iobuf_len(&b) == 11);
    memset(out, 0, sizeof(out));
    CHECK(nb_iobuf_peek(&b, 0, out, 11) == 11);
    CHECK(memcmp(out, "hello world", 11) == 0);
    CHECK(nb_iobuf_iovec(&b, iov, 4, 0) == 2);
    CHECK(iov[0].len == 5 && iov[1].len == 6);
    nb_iobuf_consume(&b, 5);
    CHECK(nb_iobuf_len(&b) == 6);
    memset(out, 0, sizeof(out));
    CHECK(nb_iobuf_peek(&b, 0, out, 6) == 6);
    CHECK(memcmp(out, " world", 6) == 0);
    nb_iobuf_consume(&b, 100);
    CHECK(nb_iobuf_empty(&b));
    nb_iobuf_reset(&b);
}

static void test_ref_and_borrow(void)
{
    nb_iobuf_t b;
    nb_buf_t *owner;
    const char borrowed[] = "borrowed";
    char out[32];

    nb_iobuf_init(&b);
    owner = nb_buf_alloc(16);
    CHECK(owner != NULL);
    if (!owner) {
        return;
    }
    memcpy(nb_buf_data(owner), "owned", 5);
    nb_buf_set_size(owner, 5);
    CHECK(nb_buf_share(owner) == 0);
    CHECK(nb_iobuf_append_buf(&b, owner, nb_buf_data(owner), 5) == 0);
    nb_buf_release(owner); /* 丢弃调用方初始引用；block 仍持有一份 */
    CHECK(nb_iobuf_append_borrow(&b, borrowed, sizeof(borrowed) - 1) == 0);
    memset(out, 0, sizeof(out));
    CHECK(nb_iobuf_peek(&b, 0, out, 13) == 13);
    CHECK(memcmp(out, "ownedborrowed", 13) == 0);
    nb_iobuf_reset(&b);
}

static void test_view(void)
{
    nb_iobuf_t b;
    nb_buf_t *scratch = NULL;
    const uint8_t *view;
    const char first[] = "abc";
    const char second[] = "def";

    nb_iobuf_init(&b);
    CHECK(nb_iobuf_append_copy(&b, first, 3) == 0);
    view = nb_iobuf_view(&b, 3, &scratch);
    CHECK(view != NULL);
    CHECK(memcmp(view, "abc", 3) == 0);
    CHECK(scratch == NULL); /* 单块连续视图不需要 scratch */
    CHECK(nb_iobuf_append_copy(&b, second, 3) == 0);
    view = nb_iobuf_view(&b, 6, &scratch);
    CHECK(view != NULL);
    CHECK(scratch != NULL);
    CHECK(memcmp(view, "abcdef", 6) == 0);
    if (scratch) {
        nb_buf_release(scratch);
    }
    nb_iobuf_reset(&b);
}

int main(void)
{
    test_copy_peek_consume();
    test_ref_and_borrow();
    test_view();
    if (g_fail) {
        nb_log_error("IOBUF FAIL");
    } else {
        nb_log_info("IOBUF NB_OK");
    }
    return g_fail ? 1 : 0;
}
