/*
 * Compile-time smoke test for the public headers. Every header is reachable as
 * <nb/...>; there is no second copy at the top of the include directory, so a
 * stray flat include would fail here rather than in a consumer.
 */
#include <nb/nb.h>
#include <nb/base.h>
#include <nb/rpc.h>
#include <nb/dds.h>
#include <nb/rpc/node.h>
#include <nb/rpc/service.h>
#include <nb/rpc/proxy.h>
#include <nb/rpc/shm.h>
#include <nb/rpc/endpoint.h>
#include <nb/rpc/transport_provider.h>
#include <nb/rpc/codec.h>
#include <nb/config.h>
#include <nb/rpc/config.h>
#include <nb/dds/config.h>
#include <nb/rpc/types.h>
#include <nb/dds/participant.h>
#include <nb/dds/topic.h>
#include <nb/dds/writer.h>
#include <nb/dds/reader.h>
#include <nb/dds/backend.h>

int main(void)
{
    nb_payload_t payload = { 0 };
    nb_endpoint_t endpoint = { 0 };
    nb_message_view_t view;

    nb_message_view_init(&view);
    nb_message_view_to_payload(&view, &payload);
    (void)endpoint;
    return payload.data == NULL && payload.len == 0 ? 0 : 1;
}
