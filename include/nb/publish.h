#ifndef NB_PUBLISH_H
#define NB_PUBLISH_H

/*
 * Publisher-facing aliases. A publisher is an ordinary proxy that sends one-way
 * EVENT messages. The second argument of nb_publisher_publish is the target
 * method for route-based transports and the topic for topic-based transports
 * such as DDS.
 *
 * Zero-copy publish (loan/borrow): providers with an inspectable payload area
 * (shm) can hand out a writable chunk directly in shared memory, so the publish
 * skips the payload copy the normal path performs. The loan pins the resolved
 * target transport at loan time and only EVENT publishing is supported.
 *
 *   nb_publisher_loan(pub, service, method, capacity, &loan, &data, &cap)
 *       Reserve a writable payload area of at least `capacity` bytes.
 *       *data points into provider memory and *cap is the actual writable
 *       capacity (>= capacity). Fill it, then publish.
 *   nb_publisher_publish_loan(loan, length, encoding, &request_id)
 *       Encode the envelope around data[0..length) and publish it as an
 *       EVENT. encoding 0 inherits the publisher's default payload_encoding
 *       (same rule as nb_proxy_call_event). Takes ownership of the loan in
 *       every case.
 *   nb_publisher_loan_release(loan)
 *       Return an unpublished loan without sending anything.
 *
 * Errors: NB_ERR_NOT_SUPPORTED when the target transport has no loan path
 * (anything but shm today); NB_ERR_AGAIN when no chunk that large is free
 * (retry once consumers drain); NB_ERR_NOTFOUND when the service cannot be
 * resolved. A loan is thread-confined: use and publish it from one thread.
 */

#include <nb/proxy.h>

typedef nb_proxy_t nb_publisher_t;
typedef nb_proxy_options_t nb_publisher_options_t;

/* Thin inline wrappers instead of macros, so these keep function-pointer and
 * debugger-friendly identities while preserving the old names. */
static inline int nb_publisher_create(nb_node_t *node,
                                      const nb_publisher_options_t *options,
                                      nb_publisher_t **out)
{
    return nb_proxy_create(node, options, out);
}

static inline void nb_publisher_destroy(nb_publisher_t *pub)
{
    nb_proxy_destroy(pub);
}

static inline int nb_publisher_publish(nb_publisher_t *pub,
                                       const char *service,
                                       const char *method,
                                       const nb_payload_t *payload,
                                       const nb_call_options_t *options)
{
    return nb_proxy_call_event(pub, service, method, payload, options);
}

/* Opaque loan handle owned by the core; freed by publish or release. */
typedef struct nb_loan nb_loan_t;

int nb_publisher_loan(nb_publisher_t *pub,
                      const char *service,
                      const char *method,
                      uint32_t capacity,
                      nb_loan_t **out_loan,
                      uint8_t **out_data,
                      uint32_t *out_capacity);
int nb_publisher_publish_loan(nb_loan_t *loan,
                              uint32_t length,
                              uint8_t encoding,
                              nb_request_id_t *out_request_id);
void nb_publisher_loan_release(nb_loan_t *loan);

#endif /* NB_PUBLISH_H */
