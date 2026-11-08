#include "rtps_param.h"

#include <string.h>

#include "rtps_cdr.h"

static bool param_writer_ok_internal(const nb_rtps_param_writer_t *writer)
{
    return writer && !writer->failed && writer->len <= writer->cap;
}

static int param_begin(nb_rtps_param_writer_t *writer, uint16_t pid, size_t *value_start)
{
    uint8_t *p;

    if (!param_writer_ok_internal(writer) || writer->cap - writer->len < 4) {
        if (writer) {
            writer->failed = 1;
        }
        return NB_ERR_TOOBIG;
    }
    p = writer->data + writer->len;
    p[0] = (uint8_t)(pid & 0xffu);
    p[1] = (uint8_t)(pid >> 8);
    p[2] = 0;
    p[3] = 0;
    writer->len += 4;
    *value_start = writer->len;
    return NB_OK;
}

static int param_end(nb_rtps_param_writer_t *writer, size_t pid_pos, size_t value_start)
{
    size_t value_len;
    size_t pad;
    size_t encoded_len;

    if (!param_writer_ok_internal(writer)) {
        return NB_ERR_TOOBIG;
    }
    value_len = writer->len - value_start;
    pad = (4u - (value_len & 3u)) & 3u;
    encoded_len = value_len + pad;
    if (encoded_len > 0xffffu) {
        writer->failed = 1;
        return NB_ERR_TOOBIG;
    }
    /* Cyclone DDS and several other implementations require the parameter
     * length to include the padding and therefore to be a multiple of 4. */
    writer->data[pid_pos + 2] = (uint8_t)(encoded_len & 0xffu);
    writer->data[pid_pos + 3] = (uint8_t)(encoded_len >> 8);
    if (pad > writer->cap - writer->len) {
        writer->failed = 1;
        return NB_ERR_TOOBIG;
    }
    memset(writer->data + writer->len, 0, pad);
    writer->len += pad;
    return NB_OK;
}

static int param_write(nb_rtps_param_writer_t *writer,
                       uint16_t pid,
                       const void *data,
                       size_t len)
{
    size_t pid_pos;
    size_t value_start;
    int rc;

    rc = param_begin(writer, pid, &value_start);
    if (rc != NB_OK) {
        return rc;
    }
    pid_pos = value_start - 4;
    if (len > 0) {
        if (!data || len > writer->cap - writer->len) {
            writer->failed = 1;
            return NB_ERR_TOOBIG;
        }
        memcpy(writer->data + writer->len, data, len);
        writer->len += len;
    }
    return param_end(writer, pid_pos, value_start);
}

void nb_rtps_param_writer_init(nb_rtps_param_writer_t *writer, void *buf, size_t cap)
{
    if (!writer) {
        return;
    }
    writer->data = (uint8_t *)buf;
    writer->cap = buf ? cap : 0;
    writer->len = 0;
    writer->failed = buf ? 0 : 1;
}

bool nb_rtps_param_writer_ok(const nb_rtps_param_writer_t *writer)
{
    return writer && !writer->failed;
}

size_t nb_rtps_param_writer_len(const nb_rtps_param_writer_t *writer)
{
    return writer ? writer->len : 0;
}

int nb_rtps_param_put_encapsulation(nb_rtps_param_writer_t *writer)
{
    if (!param_writer_ok_internal(writer) || writer->cap - writer->len < 4) {
        if (writer) {
            writer->failed = 1;
        }
        return NB_ERR_TOOBIG;
    }
    writer->data[writer->len++] = (uint8_t)(NB_RTPS_ENCAPSULATION_PL_CDR_LE >> 8);
    writer->data[writer->len++] = (uint8_t)(NB_RTPS_ENCAPSULATION_PL_CDR_LE & 0xffu);
    writer->data[writer->len++] = 0x00;
    writer->data[writer->len++] = 0x00;
    return NB_OK;
}

int nb_rtps_param_put_octets2(nb_rtps_param_writer_t *writer,
                              uint16_t pid,
                              uint8_t first,
                              uint8_t second)
{
    uint8_t pair[2];

    pair[0] = first;
    pair[1] = second;
    return param_write(writer, pid, pair, sizeof(pair));
}

int nb_rtps_param_put_u16(nb_rtps_param_writer_t *writer, uint16_t pid, uint16_t value)
{
    uint8_t buf[2];

    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)(value >> 8);
    return param_write(writer, pid, buf, sizeof(buf));
}

int nb_rtps_param_put_u32(nb_rtps_param_writer_t *writer, uint16_t pid, uint32_t value)
{
    uint8_t buf[4];

    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
    return param_write(writer, pid, buf, sizeof(buf));
}

int nb_rtps_param_put_bytes(nb_rtps_param_writer_t *writer,
                            uint16_t pid,
                            const void *data,
                            size_t len)
{
    return param_write(writer, pid, data, len);
}

int nb_rtps_param_put_resource_limits(nb_rtps_param_writer_t *writer,
                                      uint16_t pid,
                                      uint32_t max_samples,
                                      uint32_t max_instances,
                                      uint32_t max_samples_per_instance)
{
    uint8_t buf[12];
    const uint32_t values[3] = {
        max_samples, max_instances, max_samples_per_instance};
    size_t i;

    for (i = 0; i < 3u; i++) {
        /* 0 in nb_ps_qos_t means unlimited, which DDS encodes as LENGTH_UNLIMITED
         * (-1). A literal 0 is rejected by strict peers (Cyclone discards the
         * whole ParameterList), and 0 is not a legal resource limit anyway. */
        uint32_t value = values[i] == 0 ? 0xffffffffu : values[i];

        buf[i * 4u] = (uint8_t)(value & 0xffu);
        buf[i * 4u + 1] = (uint8_t)((value >> 8) & 0xffu);
        buf[i * 4u + 2] = (uint8_t)((value >> 16) & 0xffu);
        buf[i * 4u + 3] = (uint8_t)((value >> 24) & 0xffu);
    }
    return param_write(writer, pid, buf, sizeof(buf));
}

int nb_rtps_param_put_octet_sequence(nb_rtps_param_writer_t *writer,
                                     uint16_t pid,
                                     const void *data,
                                     size_t len)
{
    uint8_t length_bytes[4];
    size_t pid_pos;
    size_t value_start;
    int rc;

    if (len > 0xffffffffu) {
        return NB_ERR_TOOBIG;
    }
    rc = param_begin(writer, pid, &value_start);
    if (rc != NB_OK) {
        return rc;
    }
    pid_pos = value_start - 4;
    length_bytes[0] = (uint8_t)(len & 0xffu);
    length_bytes[1] = (uint8_t)((len >> 8) & 0xffu);
    length_bytes[2] = (uint8_t)((len >> 16) & 0xffu);
    length_bytes[3] = (uint8_t)((len >> 24) & 0xffu);
    if (4 > writer->cap - writer->len) {
        writer->failed = 1;
        return NB_ERR_TOOBIG;
    }
    memcpy(writer->data + writer->len, length_bytes, 4);
    writer->len += 4;
    if (len > 0) {
        if (!data || len > writer->cap - writer->len) {
            writer->failed = 1;
            return NB_ERR_TOOBIG;
        }
        memcpy(writer->data + writer->len, data, len);
        writer->len += len;
    }
    return param_end(writer, pid_pos, value_start);
}

int nb_rtps_param_put_string(nb_rtps_param_writer_t *writer, uint16_t pid, const char *value)
{
    nb_cdr_writer_t cdr;
    size_t pid_pos;
    size_t value_start;
    int rc;

    if (!value) {
        value = "";
    }
    rc = param_begin(writer, pid, &value_start);
    if (rc != NB_OK) {
        return rc;
    }
    pid_pos = value_start - 4;
    nb_cdr_writer_init(&cdr, writer->data + value_start, writer->cap - value_start);
    nb_cdr_put_string(&cdr, value);
    if (!nb_cdr_writer_ok(&cdr)) {
        writer->failed = 1;
        return NB_ERR_TOOBIG;
    }
    writer->len = value_start + nb_cdr_writer_len(&cdr);
    return param_end(writer, pid_pos, value_start);
}

int nb_rtps_param_put_guid(nb_rtps_param_writer_t *writer,
                           uint16_t pid,
                           const nb_rtps_guid_t *guid)
{
    uint8_t buf[NB_RTPS_GUID_SIZE];

    if (!guid) {
        return NB_ERR_INVALID;
    }
    memcpy(buf, guid->prefix.bytes, NB_RTPS_GUID_PREFIX_SIZE);
    memcpy(buf + NB_RTPS_GUID_PREFIX_SIZE, guid->entity.bytes, NB_RTPS_ENTITY_ID_SIZE);
    return param_write(writer, pid, buf, sizeof(buf));
}

static void locator_to_bytes(const nb_rtps_locator_t *locator, uint8_t out[24])
{
    out[0] = (uint8_t)((uint32_t)locator->kind & 0xffu);
    out[1] = (uint8_t)(((uint32_t)locator->kind >> 8) & 0xffu);
    out[2] = (uint8_t)(((uint32_t)locator->kind >> 16) & 0xffu);
    out[3] = (uint8_t)(((uint32_t)locator->kind >> 24) & 0xffu);
    out[4] = (uint8_t)(locator->port & 0xffu);
    out[5] = (uint8_t)((locator->port >> 8) & 0xffu);
    out[6] = (uint8_t)((locator->port >> 16) & 0xffu);
    out[7] = (uint8_t)((locator->port >> 24) & 0xffu);
    memcpy(out + 8, locator->address, NB_RTPS_LOCATOR_ADDRESS_SIZE);
}

int nb_rtps_param_put_locator(nb_rtps_param_writer_t *writer,
                              uint16_t pid,
                              const nb_rtps_locator_t *locator)
{
    uint8_t buf[24];

    if (!locator) {
        return NB_ERR_INVALID;
    }
    locator_to_bytes(locator, buf);
    return param_write(writer, pid, buf, sizeof(buf));
}

int nb_rtps_param_put_locator_sequence(nb_rtps_param_writer_t *writer,
                                       uint16_t pid,
                                       const nb_rtps_locator_t *locators,
                                       size_t count)
{
    nb_cdr_writer_t cdr;
    size_t pid_pos;
    size_t value_start;
    size_t i;
    int rc;

    if (count > 0 && !locators) {
        return NB_ERR_INVALID;
    }
    if (count > 0xffffffffu) {
        return NB_ERR_TOOBIG;
    }
    rc = param_begin(writer, pid, &value_start);
    if (rc != NB_OK) {
        return rc;
    }
    pid_pos = value_start - 4;
    nb_cdr_writer_init(&cdr, writer->data + value_start, writer->cap - value_start);
    nb_cdr_put_u32(&cdr, (uint32_t)count);
    for (i = 0; i < count; i++) {
        uint8_t bytes[24];
        locator_to_bytes(&locators[i], bytes);
        nb_cdr_put_bytes(&cdr, bytes, sizeof(bytes));
    }
    if (!nb_cdr_writer_ok(&cdr)) {
        writer->failed = 1;
        return NB_ERR_TOOBIG;
    }
    writer->len = value_start + nb_cdr_writer_len(&cdr);
    return param_end(writer, pid_pos, value_start);
}

int nb_rtps_param_put_string_sequence(nb_rtps_param_writer_t *writer,
                                      uint16_t pid,
                                      const char *const *values,
                                      size_t count)
{
    nb_cdr_writer_t cdr;
    size_t pid_pos;
    size_t value_start;
    size_t i;
    int rc;

    if (count > 0 && !values) {
        return NB_ERR_INVALID;
    }
    if (count > 0xffffffffu) {
        return NB_ERR_TOOBIG;
    }
    rc = param_begin(writer, pid, &value_start);
    if (rc != NB_OK) {
        return rc;
    }
    pid_pos = value_start - 4;
    nb_cdr_writer_init(&cdr, writer->data + value_start, writer->cap - value_start);
    nb_cdr_put_u32(&cdr, (uint32_t)count);
    for (i = 0; i < count; i++) {
        nb_cdr_put_string(&cdr, values[i] ? values[i] : "");
    }
    if (!nb_cdr_writer_ok(&cdr)) {
        writer->failed = 1;
        return NB_ERR_TOOBIG;
    }
    writer->len = value_start + nb_cdr_writer_len(&cdr);
    return param_end(writer, pid_pos, value_start);
}

int nb_rtps_param_put_history(nb_rtps_param_writer_t *writer,
                              uint16_t pid,
                              uint32_t kind,
                              uint32_t depth)
{
    uint8_t buf[8];

    buf[0] = (uint8_t)(kind & 0xffu);
    buf[1] = (uint8_t)((kind >> 8) & 0xffu);
    buf[2] = (uint8_t)((kind >> 16) & 0xffu);
    buf[3] = (uint8_t)((kind >> 24) & 0xffu);
    buf[4] = (uint8_t)(depth & 0xffu);
    buf[5] = (uint8_t)((depth >> 8) & 0xffu);
    buf[6] = (uint8_t)((depth >> 16) & 0xffu);
    buf[7] = (uint8_t)((depth >> 24) & 0xffu);
    return param_write(writer, pid, buf, sizeof(buf));
}

int nb_rtps_param_put_reliability(nb_rtps_param_writer_t *writer,
                                  uint16_t pid,
                                  uint32_t kind,
                                  const nb_rtps_duration_t *max_blocking_time)
{
    uint8_t buf[12];

    if (!max_blocking_time) {
        return NB_ERR_INVALID;
    }
    buf[0] = (uint8_t)(kind & 0xffu);
    buf[1] = (uint8_t)((kind >> 8) & 0xffu);
    buf[2] = (uint8_t)((kind >> 16) & 0xffu);
    buf[3] = (uint8_t)((kind >> 24) & 0xffu);
    buf[4] = (uint8_t)((uint32_t)max_blocking_time->seconds & 0xffu);
    buf[5] = (uint8_t)(((uint32_t)max_blocking_time->seconds >> 8) & 0xffu);
    buf[6] = (uint8_t)(((uint32_t)max_blocking_time->seconds >> 16) & 0xffu);
    buf[7] = (uint8_t)(((uint32_t)max_blocking_time->seconds >> 24) & 0xffu);
    buf[8] = (uint8_t)(max_blocking_time->fraction & 0xffu);
    buf[9] = (uint8_t)((max_blocking_time->fraction >> 8) & 0xffu);
    buf[10] = (uint8_t)((max_blocking_time->fraction >> 16) & 0xffu);
    buf[11] = (uint8_t)((max_blocking_time->fraction >> 24) & 0xffu);
    return param_write(writer, pid, buf, sizeof(buf));
}

int nb_rtps_param_put_duration(nb_rtps_param_writer_t *writer,
                               uint16_t pid,
                               const nb_rtps_duration_t *duration)
{
    uint8_t buf[8];

    if (!duration) {
        return NB_ERR_INVALID;
    }
    buf[0] = (uint8_t)((uint32_t)duration->seconds & 0xffu);
    buf[1] = (uint8_t)(((uint32_t)duration->seconds >> 8) & 0xffu);
    buf[2] = (uint8_t)(((uint32_t)duration->seconds >> 16) & 0xffu);
    buf[3] = (uint8_t)(((uint32_t)duration->seconds >> 24) & 0xffu);
    buf[4] = (uint8_t)(duration->fraction & 0xffu);
    buf[5] = (uint8_t)((duration->fraction >> 8) & 0xffu);
    buf[6] = (uint8_t)((duration->fraction >> 16) & 0xffu);
    buf[7] = (uint8_t)((duration->fraction >> 24) & 0xffu);
    return param_write(writer, pid, buf, sizeof(buf));
}

int nb_rtps_param_writer_finish(nb_rtps_param_writer_t *writer)
{
    uint8_t sentinel[4];

    sentinel[0] = (uint8_t)(NB_RTPS_PID_SENTINEL & 0xffu);
    sentinel[1] = (uint8_t)(NB_RTPS_PID_SENTINEL >> 8);
    sentinel[2] = 0;
    sentinel[3] = 0;
    return param_write(writer, NB_RTPS_PID_SENTINEL, NULL, 0);
}

void nb_rtps_param_reader_init(nb_rtps_param_reader_t *reader, const void *buf, size_t len)
{
    if (!reader) {
        return;
    }
    reader->data = (const uint8_t *)buf;
    reader->len = buf ? len : 0;
    reader->pos = 0;
    reader->failed = buf ? 0 : 1;
}

bool nb_rtps_param_reader_ok(const nb_rtps_param_reader_t *reader)
{
    return reader && !reader->failed;
}

int nb_rtps_param_reader_next(nb_rtps_param_reader_t *reader,
                              uint16_t *pid,
                              const uint8_t **value,
                              size_t *value_len)
{
    for (;;) {
        uint16_t entry_pid;
        uint16_t entry_len;
        size_t pad;

        if (!reader || reader->failed || reader->pos > reader->len ||
            reader->len - reader->pos < 4) {
            if (reader) {
                reader->failed = 1;
            }
            return NB_ERR_BAD_MESSAGE;
        }
        entry_pid = (uint16_t)((uint16_t)reader->data[reader->pos] |
                               ((uint16_t)reader->data[reader->pos + 1] << 8));
        entry_len = (uint16_t)((uint16_t)reader->data[reader->pos + 2] |
                               ((uint16_t)reader->data[reader->pos + 3] << 8));
        reader->pos += 4;
        if (entry_pid == NB_RTPS_PID_SENTINEL) {
            if (entry_len != 0) {
                reader->failed = 1;
                return NB_ERR_BAD_MESSAGE;
            }
            return 0;
        }
        if ((size_t)entry_len > reader->len - reader->pos) {
            reader->failed = 1;
            return NB_ERR_BAD_MESSAGE;
        }
        if (entry_pid == NB_RTPS_PID_PAD) {
            pad = (4u - (entry_len & 3u)) & 3u;
            if (pad > reader->len - reader->pos - entry_len) {
                reader->failed = 1;
                return NB_ERR_BAD_MESSAGE;
            }
            reader->pos += (size_t)entry_len + pad;
            continue;
        }
        if (value) {
            *value = reader->data + reader->pos;
        }
        if (value_len) {
            *value_len = entry_len;
        }
        if (pid) {
            *pid = entry_pid;
        }
        reader->pos += entry_len;
        pad = (4u - (entry_len & 3u)) & 3u;
        if (pad > reader->len - reader->pos) {
            reader->failed = 1;
            return NB_ERR_BAD_MESSAGE;
        }
        reader->pos += pad;
        return 1;
    }
}

int nb_rtps_param_get_octets2(const uint8_t *value,
                              size_t len,
                              uint8_t *first,
                              uint8_t *second)
{
    if (!value || len < 2) {
        return NB_ERR_INVALID;
    }
    if (first) {
        *first = value[0];
    }
    if (second) {
        *second = value[1];
    }
    return NB_OK;
}

int nb_rtps_param_get_u16(const uint8_t *value, size_t len, uint16_t *out)
{
    if (!value || !out || len < 2) {
        return NB_ERR_INVALID;
    }
    *out = (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
    return NB_OK;
}

int nb_rtps_param_get_u32(const uint8_t *value, size_t len, uint32_t *out)
{
    if (!value || !out || len < 4) {
        return NB_ERR_INVALID;
    }
    *out = (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) |
           ((uint32_t)value[3] << 24);
    return NB_OK;
}

int nb_rtps_param_get_resource_limits(const uint8_t *value,
                                      size_t len,
                                      uint32_t *max_samples,
                                      uint32_t *max_instances,
                                      uint32_t *max_samples_per_instance)
{
    uint32_t values[3] = {0, 0, 0};
    size_t i;

    if (!value || len < sizeof(values)) {
        return NB_ERR_INVALID;
    }
    for (i = 0; i < 3u; i++) {
        values[i] = (uint32_t)value[i * 4u] | ((uint32_t)value[i * 4u + 1] << 8) |
                    ((uint32_t)value[i * 4u + 2] << 16) |
                    ((uint32_t)value[i * 4u + 3] << 24);
        if (values[i] == 0xffffffffu) {
            values[i] = 0; /* LENGTH_UNLIMITED -> nb's "unlimited" */
        }
    }
    if (max_samples) {
        *max_samples = values[0];
    }
    if (max_instances) {
        *max_instances = values[1];
    }
    if (max_samples_per_instance) {
        *max_samples_per_instance = values[2];
    }
    return NB_OK;
}

int nb_rtps_param_get_string(const uint8_t *value, size_t len, char *out, size_t cap)
{
    nb_cdr_reader_t reader;

    if (!value) {
        return NB_ERR_INVALID;
    }
    nb_cdr_reader_init(&reader, value, len);
    if (!nb_cdr_get_string(&reader, out, cap)) {
        return NB_ERR_TOOBIG;
    }
    return NB_OK;
}

int nb_rtps_param_get_guid(const uint8_t *value, size_t len, nb_rtps_guid_t *out)
{
    if (!value || !out || len < NB_RTPS_GUID_SIZE) {
        return NB_ERR_INVALID;
    }
    memcpy(out->prefix.bytes, value, NB_RTPS_GUID_PREFIX_SIZE);
    memcpy(out->entity.bytes, value + NB_RTPS_GUID_PREFIX_SIZE, NB_RTPS_ENTITY_ID_SIZE);
    return NB_OK;
}

int nb_rtps_param_get_locator(const uint8_t *value, size_t len, nb_rtps_locator_t *out)
{
    uint32_t kind;
    uint32_t port;

    if (!value || !out || len < 24) {
        return NB_ERR_INVALID;
    }
    kind = (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) |
           ((uint32_t)value[3] << 24);
    port = (uint32_t)value[4] | ((uint32_t)value[5] << 8) | ((uint32_t)value[6] << 16) |
           ((uint32_t)value[7] << 24);
    out->kind = (int32_t)kind;
    out->port = port;
    memcpy(out->address, value + 8, NB_RTPS_LOCATOR_ADDRESS_SIZE);
    return NB_OK;
}

int nb_rtps_param_get_locator_sequence(const uint8_t *value,
                                       size_t len,
                                       nb_rtps_locator_t *out,
                                       size_t cap,
                                       size_t *count)
{
    uint32_t n;
    size_t i;

    if (!value || len < 4) {
        return NB_ERR_INVALID;
    }
    n = (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
    if ((size_t)n > (len - 4) / 24) {
        return NB_ERR_BAD_MESSAGE;
    }
    if ((size_t)n > cap || (n > 0 && !out)) {
        return NB_ERR_TOOBIG;
    }
    for (i = 0; i < (size_t)n; i++) {
        int rc = nb_rtps_param_get_locator(value + 4 + i * 24, 24, &out[i]);
        if (rc != NB_OK) {
            return rc;
        }
    }
    if (count) {
        *count = (size_t)n;
    }
    return NB_OK;
}

int nb_rtps_param_get_first_string(const uint8_t *value,
                                   size_t len,
                                   char *out,
                                   size_t cap)
{
    nb_cdr_reader_t reader;
    uint32_t count;

    if (!value || len < 4) {
        return NB_ERR_INVALID;
    }
    nb_cdr_reader_init(&reader, value, len);
    count = nb_cdr_get_u32(&reader);
    if (!nb_cdr_reader_ok(&reader)) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (count == 0) {
        if (out && cap > 0) {
            out[0] = '\0';
        }
        return NB_OK;
    }
    if (!nb_cdr_get_string(&reader, out, cap) || !nb_cdr_reader_ok(&reader)) {
        return NB_ERR_TOOBIG;
    }
    return NB_OK;
}

int nb_rtps_param_get_string_sequence_joined(const uint8_t *value,
                                             size_t len,
                                             char *out,
                                             size_t cap)
{
    nb_cdr_reader_t reader;
    char item[160];
    uint32_t count;
    uint32_t i;
    size_t used = 0;

    if (!value || len < 4) {
        return NB_ERR_INVALID;
    }
    if (out && cap > 0) {
        out[0] = '\0';
    }
    nb_cdr_reader_init(&reader, value, len);
    count = nb_cdr_get_u32(&reader);
    if (!nb_cdr_reader_ok(&reader)) {
        return NB_ERR_BAD_MESSAGE;
    }
    for (i = 0; i < count; i++) {
        size_t item_len;

        if (!nb_cdr_get_string(&reader, item, sizeof(item))) {
            /* An oversized element: stop reading, keep what fitted. */
            break;
        }
        item_len = strlen(item);
        if (!out || cap == 0 || used + item_len + (used > 0 ? 1u : 0u) >= cap) {
            continue;
        }
        if (used > 0) {
            out[used++] = ',';
        }
        memcpy(out + used, item, item_len);
        used += item_len;
        out[used] = '\0';
    }
    return NB_OK;
}

int nb_rtps_param_get_history(const uint8_t *value,
                              size_t len,
                              uint32_t *kind,
                              uint32_t *depth)
{
    int rc;

    if (!value || len < 8) {
        return NB_ERR_INVALID;
    }
    rc = nb_rtps_param_get_u32(value, len, kind);
    if (rc != NB_OK) {
        return rc;
    }
    return nb_rtps_param_get_u32(value + 4, len - 4, depth);
}

int nb_rtps_param_get_duration(const uint8_t *value, size_t len, nb_rtps_duration_t *out)
{
    uint32_t seconds;
    uint32_t fraction;

    if (!value || !out || len < 8) {
        return NB_ERR_INVALID;
    }
    seconds = (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) |
              ((uint32_t)value[3] << 24);
    fraction = (uint32_t)value[4] | ((uint32_t)value[5] << 8) | ((uint32_t)value[6] << 16) |
               ((uint32_t)value[7] << 24);
    out->seconds = (int32_t)seconds;
    out->fraction = fraction;
    return NB_OK;
}

int nb_rtps_param_get_octet_sequence(const uint8_t *value,
                                     size_t len,
                                     const uint8_t **data,
                                     size_t *data_len)
{
    uint32_t count;

    if (!value || len < 4) {
        return NB_ERR_INVALID;
    }
    count = (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) |
            ((uint32_t)value[3] << 24);
    if ((size_t)count > len - 4) {
        return NB_ERR_BAD_MESSAGE;
    }
    if (data) {
        *data = value + 4;
    }
    if (data_len) {
        *data_len = count;
    }
    return NB_OK;
}
