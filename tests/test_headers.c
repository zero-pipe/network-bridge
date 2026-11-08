/*
 * Compile-time smoke test for the public headers. Every header is reachable as
 * <nb/...>; there is no second copy at the top of the include directory, so a
 * stray flat include would fail here rather than in a consumer.
 */
#include <nb/nb.h>
#include <nb/base.h>
#include <nb/rpc.h>
#include <nb/pubsub.h>
#include <nb/node.h>
#include <nb/service.h>
#include <nb/proxy.h>
#include <nb/endpoint.h>
#include <nb/transport_provider.h>
#include <nb/codec.h>
#include <nb/config.h>
#include <nb/types.h>
#include <nb/dds.h>

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
