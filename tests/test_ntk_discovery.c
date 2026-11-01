#define _CRT_SECURE_NO_WARNINGS

/*
 * Tests for the standalone ntk_discovery library. Only ntk and the library are
 * used here, which is the point: discovery must not depend on the framework.
 */

#include <ntk_discovery/ntk_discovery.h>

#include <ntk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

#define PORT_A 48300
#define PORT_B 48301

typedef struct poller_loop {
    ntk_poller *poller;
    volatile int stop;
} poller_loop_t;

typedef struct rx_state {
    ntk_mutex *lock;
    int peers;
    int peers_removed;
    int keys;
    int keys_removed;
    char last_key[NTK_DISC_KEY_MAX];
    uint8_t last_value[NTK_DISC_VALUE_MAX];
    size_t last_value_len;
} rx_state_t;

static void poller_main(void *arg)
{
    poller_loop_t *loop = (poller_loop_t *)arg;
    ntk_poller_run(loop->poller, &loop->stop);
}

static void on_peer(void *user, const ntk_disc_peer_info_t *peer, int removed)
{
    rx_state_t *rx = (rx_state_t *)user;

    (void)peer;
    ntk_mutex_lock(rx->lock);
    if (removed) {
        rx->peers_removed++;
    } else {
        rx->peers++;
    }
    ntk_mutex_unlock(rx->lock);
}

static void on_key(void *user, const ntk_disc_key_info_t *key, int removed)
{
    rx_state_t *rx = (rx_state_t *)user;

    ntk_mutex_lock(rx->lock);
    if (removed) {
        rx->keys_removed++;
    } else {
        snprintf(rx->last_key, sizeof(rx->last_key), "%s", key->key);
        rx->last_value_len = key->value_len;
        if (key->value_len > 0) {
            memcpy(rx->last_value, key->value, key->value_len);
        }
        rx->keys++;
    }
    ntk_mutex_unlock(rx->lock);
}

static int key_value_is(ntk_discovery_t *d,
                        const char *key,
                        const char *value)
{
    ntk_disc_key_info_t info;
    size_t n = ntk_discovery_lookup(d, key, &info, 1);

    if (n != 1) {
        return 0;
    }
    if (!value) {
        return 1;
    }
    return info.value_len == strlen(value) &&
           memcmp(info.value, value, info.value_len) == 0;
}

static void make_config(ntk_discovery_config_t *cfg,
                        uint16_t port,
                        ntk_discovery_callbacks_t callbacks,
                        void *user)
{
    ntk_discovery_config_init(cfg);
    cfg->bind_host = "127.0.0.1";
    cfg->port = port;
    cfg->broadcast = "127.0.0.1";
    cfg->hello_interval_ms = 1000;
    cfg->ttl_ms = 3000;
    cfg->max_peers = 64;
    cfg->max_keys = 4096;
    cfg->callbacks = callbacks;
    cfg->user = user;
}

int main(void)
{
    poller_loop_t loop;
    rx_state_t rx;
    ntk_discovery_config_t cfg_a;
    ntk_discovery_config_t cfg_b;
    ntk_discovery_callbacks_t cbs_b;
    ntk_discovery_callbacks_t cbs_none;
    ntk_discovery_t *a = NULL;
    ntk_discovery_t *b = NULL;
    ntk_thread *thread = NULL;
    ntk_disc_endpoint_t eps[1];
    ntk_disc_peer_info_t peer;
    uint8_t instance_a[NTK_DISC_INSTANCE_SIZE];
    uint8_t instance_b[NTK_DISC_INSTANCE_SIZE];
    int waited;
    size_t n;
    int i;

    memset(&rx, 0, sizeof(rx));
    rx.lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    CHECK(rx.lock != NULL);
    if (!rx.lock) {
        return 1;
    }

    memset(&loop, 0, sizeof(loop));
    loop.poller = ntk_poller_create();
    CHECK(loop.poller != NULL);
    if (!loop.poller) {
        goto out;
    }
    thread = ntk_thread_create(poller_main, &loop);
    CHECK(thread != NULL);
    if (!thread) {
        goto out;
    }

    memset(instance_a, 0, sizeof(instance_a));
    memset(instance_b, 0, sizeof(instance_b));
    instance_a[0] = 1;
    instance_b[0] = 2;

    memset(&cbs_b, 0, sizeof(cbs_b));
    cbs_b.on_peer = on_peer;
    cbs_b.on_key = on_key;
    memset(&cbs_none, 0, sizeof(cbs_none));
    make_config(&cfg_a, PORT_A, cbs_none, NULL);
    make_config(&cfg_b, PORT_B, cbs_b, &rx);

    a = ntk_discovery_create(loop.poller, &cfg_a);
    b = ntk_discovery_create(loop.poller, &cfg_b);
    CHECK(a != NULL && b != NULL);
    if (!a || !b) {
        goto out;
    }
    CHECK(ntk_discovery_port(a) == PORT_A);
    CHECK(ntk_discovery_port(b) == PORT_B);
    CHECK(ntk_discovery_set_identity(a, instance_a, "node-a") == NTK_DISC_OK);
    CHECK(ntk_discovery_set_identity(b, instance_b, "node-b") == NTK_DISC_OK);

    memset(eps, 0, sizeof(eps));
    eps[0].kind = 7;
    snprintf(eps[0].uri, sizeof(eps[0].uri), "tcp://127.0.0.1:9000");
    CHECK(ntk_discovery_set_endpoints(a, eps, 1) == NTK_DISC_OK);
    CHECK(ntk_discovery_add_peer(a, "127.0.0.1", PORT_B) == NTK_DISC_OK);
    CHECK(ntk_discovery_add_peer(b, "127.0.0.1", PORT_A) == NTK_DISC_OK);

    CHECK(ntk_discovery_advertise(a, "Svc/m1", "v1", 2, 0) == NTK_DISC_OK);

    for (waited = 0;
         waited < 5000 && !(ntk_discovery_peer_count(b) == 1 &&
                            key_value_is(b, "Svc/m1", "v1"));
         waited += 20) {
        (void)ntk_discovery_announce(a);
        (void)ntk_discovery_announce(b);
        ntk_discovery_tick(a, ntk_wall_ms());
        ntk_discovery_tick(b, ntk_wall_ms());
        ntk_sleep_ms(20);
    }
    CHECK(ntk_discovery_peer_count(b) == 1);
    CHECK(key_value_is(b, "Svc/m1", "v1"));

    CHECK(ntk_discovery_peers(b, &peer, 1) == 1);
    CHECK(memcmp(peer.instance, instance_a, sizeof(instance_a)) == 0);
    CHECK(strcmp(peer.name, "node-a") == 0);
    CHECK(strcmp(peer.host, "127.0.0.1") == 0);
    CHECK(peer.endpoint_count == 1);
    CHECK(strcmp(peer.endpoints[0].uri, "tcp://127.0.0.1:9000") == 0);

    /* Value update. */
    CHECK(ntk_discovery_advertise(a, "Svc/m1", "value-two", 9, 0) == NTK_DISC_OK);
    for (waited = 0; waited < 5000 && !key_value_is(b, "Svc/m1", "value-two");
         waited += 20) {
        (void)ntk_discovery_announce(a);
        ntk_discovery_tick(b, ntk_wall_ms());
        ntk_sleep_ms(20);
    }
    CHECK(key_value_is(b, "Svc/m1", "value-two"));

    /* Withdraw. */
    CHECK(ntk_discovery_withdraw(a, "Svc/m1") == NTK_DISC_OK);
    for (waited = 0; waited < 5000 && key_value_is(b, "Svc/m1", NULL);
         waited += 20) {
        (void)ntk_discovery_announce(a);
        ntk_discovery_tick(b, ntk_wall_ms());
        ntk_sleep_ms(20);
    }
    CHECK(!key_value_is(b, "Svc/m1", NULL));

    /* Fragmentation: 40 keys with 64-byte values do not fit in one datagram. */
    for (i = 0; i < 40; i++) {
        char key[NTK_DISC_KEY_MAX];
        uint8_t value[64];

        snprintf(key, sizeof(key), "batch/key-%02d", i);
        memset(value, (int)(0x40 + i), sizeof(value));
        CHECK(ntk_discovery_advertise(a, key, value, sizeof(value), 0) == NTK_DISC_OK);
    }
    for (waited = 0; waited < 8000; waited += 20) {
        ntk_disc_key_info_t keys[64];

        (void)ntk_discovery_announce(a);
        ntk_discovery_tick(b, ntk_wall_ms());
        n = ntk_discovery_peer_keys(b, instance_a, keys, 64);
        if (n == 40) {
            break;
        }
        ntk_sleep_ms(20);
    }
    CHECK(n == 40);
    CHECK(key_value_is(b, "batch/key-00", NULL));

    /* BYE removes the peer and its keys. */
    CHECK(ntk_discovery_bye(a) == NTK_DISC_OK);
    for (waited = 0; waited < 3000 && ntk_discovery_peer_count(b) != 0;
         waited += 20) {
        ntk_discovery_tick(b, ntk_wall_ms());
        ntk_sleep_ms(20);
    }
    CHECK(ntk_discovery_peer_count(b) == 0);
    {
        int removed;

        ntk_mutex_lock(rx.lock);
        removed = rx.peers_removed;
        ntk_mutex_unlock(rx.lock);
        CHECK(removed >= 1);
    }

out:
    if (b) {
        ntk_discovery_destroy(b);
    }
    if (a) {
        ntk_discovery_destroy(a);
    }
    if (thread) {
        loop.stop = 1;
        if (loop.poller) {
            (void)ntk_poller_wake(loop.poller);
        }
        (void)ntk_thread_join(thread);
        ntk_thread_destroy(thread);
    }
    if (loop.poller) {
        ntk_poller_destroy(loop.poller);
    }
    if (rx.lock) {
        ntk_mutex_destroy(rx.lock);
    }
    if (g_fail) {
        fprintf(stderr, "ntk_discovery test failed\n");
        return 1;
    }
    printf("ntk_discovery test passed\n");
    return 0;
}
