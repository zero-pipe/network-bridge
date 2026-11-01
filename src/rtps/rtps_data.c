#include "rtps_data.h"

#include "rtps_cdr.h"

#define NB_RTPS_CDR_LE 0x0001u
int nb_rtps_dds_message_serialize(const nb_rtps_dds_message_t *message,
                                  void *buf,
                                  size_t cap,
                                  size_t *out_len)
{
    uint8_t *encapsulated = (uint8_t *)buf;
    nb_cdr_writer_t writer;

    if (!message || !buf || !out_len) {
        return NB_ERR_INVALID;
    }
    *out_len = 0;
    if (message->payload_len > UINT32_MAX) {
        return NB_ERR_TOOBIG;
    }
    if (message->payload_len > 0 && !message->payload) {
        return NB_ERR_INVALID;
    }
    if (cap < NB_RTPS_CDR_HEADER_SIZE) {
        return NB_ERR_TOOBIG;
    }

    encapsulated[0] = (uint8_t)(NB_RTPS_CDR_LE >> 8);
    encapsulated[1] = (uint8_t)(NB_RTPS_CDR_LE & 0xffu);
    encapsulated[2] = 0;
    encapsulated[3] = 0;

    /* CDR alignment is relative to the start of the encapsulation body. */
    nb_cdr_writer_init(&writer, encapsulated + NB_RTPS_CDR_HEADER_SIZE,
                       cap - NB_RTPS_CDR_HEADER_SIZE);
    nb_cdr_put_u32(&writer, (uint32_t)message->payload_len);
    nb_cdr_put_bytes(&writer, message->payload, message->payload_len);
    if (!nb_cdr_writer_ok(&writer)) {
        return NB_ERR_TOOBIG;
    }
    *out_len = NB_RTPS_CDR_HEADER_SIZE + nb_cdr_writer_len(&writer);
    return NB_OK;
}

int nb_rtps_dds_message_deserialize(const void *buf,
                                    size_t len,
                                    nb_rtps_dds_message_t *out)
{
    const uint8_t *encapsulated = (const uint8_t *)buf;
    nb_cdr_reader_t reader;
    const uint8_t *payload;
    uint32_t payload_len;

    if (!encapsulated || !out || len < NB_RTPS_CDR_HEADER_SIZE) {
        return NB_ERR_INVALID;
    }
    /*
     * The first two bytes are the CDR encapsulation identifier (0x0001 for
     * CDR_LE). The remaining two bytes are option bits. Cyclone DDS emits
     * 0x0001 here, so accept any options and treat the body as CDR_LE.
     */
    if (encapsulated[0] != 0x00 || encapsulated[1] != 0x01) {
        return NB_ERR_NOT_SUPPORTED;
    }

    out->payload = NULL;
    out->payload_len = 0;
    nb_cdr_reader_init(&reader, encapsulated + NB_RTPS_CDR_HEADER_SIZE,
                       len - NB_RTPS_CDR_HEADER_SIZE);
    payload_len = nb_cdr_get_u32(&reader);
    payload = nb_cdr_get_bytes(&reader, payload_len);
    if (!nb_cdr_reader_ok(&reader) || !payload) {
        return NB_ERR_BAD_MESSAGE;
    }

    /* Keep the payload as a zero-copy view into the caller's buffer. */
    out->payload = payload;
    out->payload_len = payload_len;
    return NB_OK;
}
