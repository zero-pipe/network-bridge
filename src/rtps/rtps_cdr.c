#include "rtps_cdr.h"

#include <string.h>

static int writer_bad(const nb_cdr_writer_t *writer)
{
    return !writer || writer->failed || writer->buffer.error;
}

static int reader_bad(const nb_cdr_reader_t *reader)
{
    return !reader || reader->failed || reader->buffer.error;
}

/*
 * Pad to the next multiple of alignment. Micro-CDR advances over implicit
 * padding without writing it, so the padding is zeroed here before the
 * advance: encoded frames must not depend on the previous contents of the
 * destination buffer.
 */
static void writer_align(nb_cdr_writer_t *writer, size_t alignment)
{
    size_t pad;

    if (writer_bad(writer) || alignment == 0) {
        return;
    }
    pad = ucdr_buffer_alignment(&writer->buffer, alignment);
    if (pad > ucdr_buffer_remaining(&writer->buffer)) {
        writer->failed = 1;
        return;
    }
    if (pad > 0) {
        memset(writer->buffer.iterator, 0, pad);
    }
    ucdr_align_to(&writer->buffer, alignment);
}

static void reader_align(nb_cdr_reader_t *reader, size_t alignment)
{
    size_t pad;

    if (reader_bad(reader) || alignment == 0) {
        return;
    }
    pad = ucdr_buffer_alignment(&reader->buffer, alignment);
    if (pad > ucdr_buffer_remaining(&reader->buffer)) {
        reader->failed = 1;
        return;
    }
    ucdr_align_to(&reader->buffer, alignment);
}

void nb_cdr_writer_init(nb_cdr_writer_t *writer, void *buf, size_t cap)
{
    if (!writer) {
        return;
    }
    memset(writer, 0, sizeof(*writer));
    ucdr_init_buffer_origin_offset_endian(&writer->buffer,
                                         (uint8_t *)buf,
                                         buf ? cap : 0,
                                         0,
                                         0,
                                         UCDR_LITTLE_ENDIANNESS);
    if (!buf) {
        writer->failed = 1;
        writer->buffer.error = true;
    }
}

void nb_cdr_reader_init(nb_cdr_reader_t *reader, const void *buf, size_t len)
{
    if (!reader) {
        return;
    }
    memset(reader, 0, sizeof(*reader));
    /* Micro-CDR takes a mutable pointer; deserialisation only reads. */
    ucdr_init_buffer_origin_offset_endian(&reader->buffer,
                                         (uint8_t *)(uintptr_t)buf,
                                         buf ? len : 0,
                                         0,
                                         0,
                                         UCDR_LITTLE_ENDIANNESS);
    if (!buf) {
        reader->failed = 1;
        reader->buffer.error = true;
    }
}

bool nb_cdr_writer_ok(const nb_cdr_writer_t *writer)
{
    return !writer_bad(writer);
}

bool nb_cdr_reader_ok(const nb_cdr_reader_t *reader)
{
    return !reader_bad(reader);
}

size_t nb_cdr_writer_len(const nb_cdr_writer_t *writer)
{
    if (!writer) {
        return 0;
    }
    return ucdr_buffer_length(&writer->buffer);
}

uint8_t *nb_cdr_writer_data(nb_cdr_writer_t *writer)
{
    if (writer_bad(writer)) {
        return NULL;
    }
    return writer->buffer.init;
}

size_t nb_cdr_reader_remaining(const nb_cdr_reader_t *reader)
{
    if (reader_bad(reader)) {
        return 0;
    }
    return ucdr_buffer_remaining(&reader->buffer);
}

void nb_cdr_writer_align(nb_cdr_writer_t *writer, size_t alignment)
{
    writer_align(writer, alignment);
}

void nb_cdr_reader_align(nb_cdr_reader_t *reader, size_t alignment)
{
    reader_align(reader, alignment);
}

void nb_cdr_put_u8(nb_cdr_writer_t *writer, uint8_t value)
{
    if (writer_bad(writer)) {
        return;
    }
    if (!ucdr_serialize_uint8_t(&writer->buffer, value)) {
        writer->failed = 1;
    }
}

void nb_cdr_put_u16(nb_cdr_writer_t *writer, uint16_t value)
{
    writer_align(writer, 2);
    if (writer_bad(writer)) {
        return;
    }
    if (!ucdr_serialize_uint16_t(&writer->buffer, value)) {
        writer->failed = 1;
    }
}

void nb_cdr_put_u32(nb_cdr_writer_t *writer, uint32_t value)
{
    writer_align(writer, 4);
    if (writer_bad(writer)) {
        return;
    }
    if (!ucdr_serialize_uint32_t(&writer->buffer, value)) {
        writer->failed = 1;
    }
}

void nb_cdr_put_u64(nb_cdr_writer_t *writer, uint64_t value)
{
    writer_align(writer, 8);
    if (writer_bad(writer)) {
        return;
    }
    if (!ucdr_serialize_uint64_t(&writer->buffer, value)) {
        writer->failed = 1;
    }
}

void nb_cdr_put_i32(nb_cdr_writer_t *writer, int32_t value)
{
    nb_cdr_put_u32(writer, (uint32_t)value);
}

void nb_cdr_put_bytes(nb_cdr_writer_t *writer, const void *data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (writer_bad(writer)) {
        return;
    }
    if (!data) {
        writer->failed = 1;
        return;
    }
    if (!ucdr_serialize_array_uint8_t(
            &writer->buffer, (const uint8_t *)data, len)) {
        writer->failed = 1;
    }
}

void nb_cdr_put_string(nb_cdr_writer_t *writer, const char *value)
{
    if (writer_bad(writer)) {
        return;
    }
    if (!value) {
        value = "";
    }
    writer_align(writer, 4);
    if (writer_bad(writer)) {
        return;
    }
    if (!ucdr_serialize_string(&writer->buffer, value)) {
        writer->failed = 1;
    }
}

uint8_t nb_cdr_get_u8(nb_cdr_reader_t *reader)
{
    uint8_t value = 0;

    if (reader_bad(reader)) {
        return 0;
    }
    if (!ucdr_deserialize_uint8_t(&reader->buffer, &value)) {
        reader->failed = 1;
        return 0;
    }
    return value;
}

uint16_t nb_cdr_get_u16(nb_cdr_reader_t *reader)
{
    uint16_t value = 0;

    reader_align(reader, 2);
    if (reader_bad(reader)) {
        return 0;
    }
    if (!ucdr_deserialize_uint16_t(&reader->buffer, &value)) {
        reader->failed = 1;
        return 0;
    }
    return value;
}

uint32_t nb_cdr_get_u32(nb_cdr_reader_t *reader)
{
    uint32_t value = 0;

    reader_align(reader, 4);
    if (reader_bad(reader)) {
        return 0;
    }
    if (!ucdr_deserialize_uint32_t(&reader->buffer, &value)) {
        reader->failed = 1;
        return 0;
    }
    return value;
}

uint64_t nb_cdr_get_u64(nb_cdr_reader_t *reader)
{
    uint64_t value = 0;

    reader_align(reader, 8);
    if (reader_bad(reader)) {
        return 0;
    }
    if (!ucdr_deserialize_uint64_t(&reader->buffer, &value)) {
        reader->failed = 1;
        return 0;
    }
    return value;
}

int32_t nb_cdr_get_i32(nb_cdr_reader_t *reader)
{
    return (int32_t)nb_cdr_get_u32(reader);
}

const uint8_t *nb_cdr_get_bytes(nb_cdr_reader_t *reader, size_t len)
{
    const uint8_t *p;

    if (reader_bad(reader)) {
        return NULL;
    }
    if (len > ucdr_buffer_remaining(&reader->buffer)) {
        reader->failed = 1;
        return NULL;
    }
    /* Zero-copy: hand out the position inside the caller's buffer. */
    p = reader->buffer.iterator;
    reader->buffer.iterator += len;
    reader->buffer.offset += len;
    reader->buffer.last_data_size = 1;
    return p;
}

bool nb_cdr_get_string(nb_cdr_reader_t *reader, char *out, size_t cap)
{
    uint32_t wire_len;
    const uint8_t *p;
    size_t copy_len;

    if (reader_bad(reader)) {
        return false;
    }
    wire_len = nb_cdr_get_u32(reader);
    if (reader_bad(reader)) {
        return false;
    }
    if (wire_len == 0) {
        reader->failed = 1;
        return false;
    }
    p = nb_cdr_get_bytes(reader, (size_t)wire_len);
    if (!p) {
        return false;
    }
    copy_len = (size_t)wire_len - 1;
    if (out && cap > 0) {
        if (copy_len >= cap) {
            reader->failed = 1;
            return false;
        }
        if (copy_len > 0) {
            memcpy(out, p, copy_len);
        }
        out[copy_len] = '\0';
    } else if (copy_len > 0) {
        reader->failed = 1;
        return false;
    }
    return true;
}
