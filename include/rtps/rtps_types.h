#ifndef RTPS_TYPES_H
#define RTPS_TYPES_H

/*
 * Wire-level value types shared by the public API. Their memory layout matches
 * what the engine uses, so handles and GUIDs can be compared and stored by the
 * caller; they are plain data, not entities.
 */

#include <stddef.h>
#include <stdint.h>

#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTPS_GUID_PREFIX_SIZE 12u
#define RTPS_ENTITY_ID_SIZE 4u
#define RTPS_GUID_SIZE 16u
#define RTPS_LOCATOR_ADDRESS_SIZE 16u
#define RTPS_INSTANCE_HANDLE_SIZE 16u
#define RTPS_PARTITION_MAX 128u

typedef struct rtps_guid_prefix {
    uint8_t bytes[RTPS_GUID_PREFIX_SIZE];
} rtps_guid_prefix_t;

typedef struct rtps_entity_id {
    uint8_t bytes[RTPS_ENTITY_ID_SIZE];
} rtps_entity_id_t;

typedef struct rtps_guid {
    rtps_guid_prefix_t prefix;
    rtps_entity_id_t entity;
} rtps_guid_t;

typedef enum rtps_locator_kind {
    RTPS_LOCATOR_INVALID = 0,
    RTPS_LOCATOR_UDPV4 = 1,
    RTPS_LOCATOR_UDPV6 = 2,
    RTPS_LOCATOR_TCPV4 = 4,
    RTPS_LOCATOR_TCPV6 = 8
} rtps_locator_kind_t;

typedef struct rtps_locator {
    int32_t kind; /* rtps_locator_kind_t */
    uint32_t port;
    uint8_t address[RTPS_LOCATOR_ADDRESS_SIZE];
} rtps_locator_t;

/* RTPS Time_t: seconds plus a fraction in units of 2^-32 s. */
typedef struct rtps_time {
    int32_t seconds;
    uint32_t fraction;
} rtps_time_t;

typedef struct rtps_instance_handle {
    uint8_t bytes[RTPS_INSTANCE_HANDLE_SIZE];
} rtps_instance_handle_t;

/* The state of the instance a sample belongs to (DDS InstanceStateKind). */
typedef enum rtps_instance_state {
    RTPS_INSTANCE_ALIVE = 0,
    RTPS_INSTANCE_NOT_ALIVE_DISPOSED = 1,
    RTPS_INSTANCE_NOT_ALIVE_NO_WRITERS = 2
} rtps_instance_state_t;

/* DDS ViewStateKind: NEW until the first sample of that instance is seen. */
typedef enum rtps_view_state {
    RTPS_VIEW_STATE_NEW = 0,
    RTPS_VIEW_STATE_NOT_NEW = 1
} rtps_view_state_t;

/* DDS SampleStateKind. */
typedef enum rtps_sample_state {
    RTPS_SAMPLE_STATE_READ = 0,
    RTPS_SAMPLE_STATE_NOT_READ = 1
} rtps_sample_state_t;

/* Bit masks for rtps_sample_filter_t. A dimension only constrains the match
 * when its mask is not RTPS_STATE_MASK_ANY. */
#define RTPS_STATE_MASK_ANY 0xffffffffu
#define RTPS_SAMPLE_STATE_READ_BIT 0x1u
#define RTPS_SAMPLE_STATE_NOT_READ_BIT 0x2u
#define RTPS_VIEW_STATE_NEW_BIT 0x1u
#define RTPS_VIEW_STATE_NOT_NEW_BIT 0x2u
#define RTPS_INSTANCE_STATE_ALIVE_BIT 0x1u
#define RTPS_INSTANCE_STATE_DISPOSED_BIT 0x2u
#define RTPS_INSTANCE_STATE_NO_WRITERS_BIT 0x4u

typedef struct rtps_sample_info {
    /* False for a dispose/unregister sample: data may still point at the keys. */
    int valid_data;
    rtps_instance_state_t instance_state;
    rtps_view_state_t view_state;
    rtps_sample_state_t sample_state;
    int has_instance_handle;
    rtps_instance_handle_t instance_handle;
    /* Source (writer) and reception timestamps, nanoseconds since the epoch. */
    int64_t source_timestamp_ns;
    int64_t reception_timestamp_ns;
    /* DDS generationRank: how often the instance came back alive. */
    uint32_t generation_rank;
    /* Low 32 bits of the writer sequence number. */
    uint32_t writer_sn;
} rtps_sample_info_t;

/*
 * One sample handed to a callback or returned by read/take.
 *
 * In a callback the bytes are a zero-copy view valid only for the duration of
 * the call. read() returns views owned by the reader (valid until the next
 * take/destroy/enqueue), take() returns owned copies that the caller releases
 * with rtps_sample_release().
 *
 * RTPS pads each submessage to a 4-octet boundary and an opaque CDR body has no
 * length field of its own, so `size` can be up to three octets longer than the
 * value the writer wrote. A decoder must ignore trailing bytes, exactly as it
 * does when the same body arrives from Cyclone DDS.
 */
typedef struct rtps_sample {
    const uint8_t *data;
    size_t size;
    rtps_sample_info_t info;
} rtps_sample_t;

/*
 * Filter for read/take/wait. Initialise with rtps_sample_filter_init(); the
 * default accepts every sample and every instance.
 */
typedef struct rtps_sample_filter {
    uint32_t sample_state_mask;
    uint32_t view_state_mask;
    uint32_t instance_state_mask;
    int has_instance_handle;
    rtps_instance_handle_t instance_handle;
} rtps_sample_filter_t;

RTPS_API void rtps_sample_filter_init(rtps_sample_filter_t *filter);
/* Restrict the filter to one instance; pass NULL to clear. */
RTPS_API void rtps_sample_filter_set_instance(rtps_sample_filter_t *filter,
                                              const rtps_instance_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_TYPES_H */
