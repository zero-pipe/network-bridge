#ifndef NB_MESSAGE_VIEW_H
#define NB_MESSAGE_VIEW_H

/*
 * Unified message view.
 *
 * Mirrors fc_message_view from fastcall-c: requests and responses share one
 * payload-carrying struct and differ only in interpretation. A view may borrow
 * external memory (borrowed) or own a copy (owned, created by set_data /
 * set_string); nb_message_view_release() frees owned buffers.
 *
 * Main use: synchronous-style handlers (see nb_service_add_sync_method) write
 * their response into a response view, and the framework frees it after the
 * callback returns.
 */

#include <nb/types.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- View struct ---- */
typedef struct nb_message_view {
    const void *data; /* byte view */
    size_t len;       /* byte count */
    uint8_t encoding; /* application encoding tag; 0 = raw bytes */
    nb_buf_t *owner;  /* internal: ref-counted buffer owned by set_*; do not modify */
} nb_message_view_t;

/* ---- Lifecycle ---- */
/* Initialize to an empty view. */
void nb_message_view_init(nb_message_view_t *view);
/* Free the internal buffer and clear the view. Safe on borrowed views. */
void nb_message_view_release(nb_message_view_t *view);

/* ---- Writing ---- */
/* Borrow external memory (no copy, no ownership transfer). */
int nb_message_view_set_borrowed(nb_message_view_t *view, const void *data, size_t len);

/* Copy memory into a buffer owned by the view; a second call replaces it. */
int nb_message_view_set_data(nb_message_view_t *view, const void *data, size_t len);
/* Copy a NUL-terminated string (excluding the terminator) into the view. */
int nb_message_view_set_string(nb_message_view_t *view, const char *text);
/* Set from a payload: copies data into the view and copies encoding. */
int nb_message_view_set_payload(nb_message_view_t *view, const nb_payload_t *payload);

/* ---- Export ---- */
/* Export as a payload (data points into the view buffer and is valid only while
 * the view lives). */
void nb_message_view_to_payload(const nb_message_view_t *view, nb_payload_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_MESSAGE_VIEW_H */
