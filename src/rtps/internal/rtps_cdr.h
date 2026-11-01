#ifndef NB_RTPS_CDR_H
#define NB_RTPS_CDR_H

/*
 * Little-endian CDR reader/writer used by the RTPS codec, implemented on top
 * of Micro-CDR (ucdrBuffer).
 *
 * Alignment is relative to the start of the buffer passed to init. This lets
 * each ParameterList value be decoded as its own CDR stream, which is how
 * RTPS parameter values are aligned.
 *
 * Alignment padding is written as zeroes, so encoded frames are deterministic
 * and never leak uninitialised bytes. The API is bounds checked: the first
 * failure sticks and later calls become no-ops. Callers check nb_cdr_*_ok()
 * once at the end.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ucdr/microcdr.h>

typedef struct nb_cdr_writer {
    ucdrBuffer buffer;
    int failed;
} nb_cdr_writer_t;

typedef struct nb_cdr_reader {
    ucdrBuffer buffer;
    int failed;
} nb_cdr_reader_t;

void nb_cdr_writer_init(nb_cdr_writer_t *writer, void *buf, size_t cap);
void nb_cdr_reader_init(nb_cdr_reader_t *reader, const void *buf, size_t len);

bool nb_cdr_writer_ok(const nb_cdr_writer_t *writer);
bool nb_cdr_reader_ok(const nb_cdr_reader_t *reader);
size_t nb_cdr_writer_len(const nb_cdr_writer_t *writer);
size_t nb_cdr_reader_remaining(const nb_cdr_reader_t *reader);

/* Mutable view of the bytes written so far. Only use it to patch a field in
 * place (for example a submessage length) while the writer is still valid. */
uint8_t *nb_cdr_writer_data(nb_cdr_writer_t *writer);

/* Pad with zeroes to the next multiple of alignment (must be a power of 2). */
void nb_cdr_writer_align(nb_cdr_writer_t *writer, size_t alignment);
void nb_cdr_reader_align(nb_cdr_reader_t *reader, size_t alignment);

void nb_cdr_put_u8(nb_cdr_writer_t *writer, uint8_t value);
void nb_cdr_put_u16(nb_cdr_writer_t *writer, uint16_t value);
void nb_cdr_put_u32(nb_cdr_writer_t *writer, uint32_t value);
void nb_cdr_put_u64(nb_cdr_writer_t *writer, uint64_t value);
void nb_cdr_put_i32(nb_cdr_writer_t *writer, int32_t value);
void nb_cdr_put_bytes(nb_cdr_writer_t *writer, const void *data, size_t len);
void nb_cdr_put_string(nb_cdr_writer_t *writer, const char *value);

uint8_t nb_cdr_get_u8(nb_cdr_reader_t *reader);
uint16_t nb_cdr_get_u16(nb_cdr_reader_t *reader);
uint32_t nb_cdr_get_u32(nb_cdr_reader_t *reader);
uint64_t nb_cdr_get_u64(nb_cdr_reader_t *reader);
int32_t nb_cdr_get_i32(nb_cdr_reader_t *reader);
const uint8_t *nb_cdr_get_bytes(nb_cdr_reader_t *reader, size_t len);
/* Copies a CDR string (u32 length including NUL) into out. Fails when the
 * string does not fit; cap == 0 with out == NULL only skips the copy. */
bool nb_cdr_get_string(nb_cdr_reader_t *reader, char *out, size_t cap);

#endif /* NB_RTPS_CDR_H */
