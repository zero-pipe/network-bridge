/* 传输抽象层的便捷包装。 */
#include "transport_internal.h"

#include <stdlib.h>

int nb_transport_start(nb_transport_t *t)
{
    if (!t || !t->ops || !t->ops->start) {
        return NB_ERR_INVALID;
    }
    return t->ops->start(t);
}

void nb_transport_stop(nb_transport_t *t)
{
    if (t && t->ops && t->ops->stop) {
        t->ops->stop(t);
    }
}

void nb_transport_destroy(nb_transport_t *t)
{
    if (!t) {
        return;
    }
    nb_transport_stop(t);
    if (t->provider && t->provider_instance) {
        t->provider->ops->destroy(t->provider_instance);
        t->provider_instance = NULL;
    }
    free(t);
}

int nb_transport_send(nb_transport_t *t, const nb_message_t *message)
{
    if (!t || !t->ops || !t->ops->send || !message) {
        return NB_ERR_INVALID;
    }
    return t->ops->send(t, message);
}

int nb_transport_reply(nb_transport_t *t, const nb_message_t *message)
{
    if (!t || !t->ops || !t->ops->reply || !message) {
        return NB_ERR_INVALID;
    }
    return t->ops->reply(t, message);
}

int nb_transport_loan(nb_transport_t *t,
                      const char *route,
                      uint32_t payload_capacity,
                      void **payload_data,
                      uint32_t *payload_capacity_out,
                      void **loan)
{
    if (!t || !t->ops || !t->ops->loan) {
        return NB_ERR_NOT_SUPPORTED;
    }
    return t->ops->loan(t, route, payload_capacity, payload_data, payload_capacity_out, loan);
}

int nb_transport_send_loan(nb_transport_t *t,
                           const char *route,
                           void *loan,
                           const nb_message_t *message)
{
    if (!t || !t->ops || !t->ops->send_loan) {
        return NB_ERR_NOT_SUPPORTED;
    }
    return t->ops->send_loan(t, route, loan, message);
}

void nb_transport_release_loan(nb_transport_t *t, void *loan)
{
    if (t && t->ops && t->ops->release_loan) {
        t->ops->release_loan(t, loan);
    }
}

int nb_transport_subscribe(nb_transport_t *t, const char *service, const char *method)
{
    if (!t || !service || !method) {
        return NB_ERR_INVALID;
    }
    if (!t->provider || !t->provider_instance || !t->provider->ops->register_method) {
        return NB_OK;
    }
    return t->provider->ops->register_method(t->provider_instance, service, method);
}

void nb_transport_unsubscribe(nb_transport_t *t, const char *service, const char *method)
{
    if (!t || !service || !method || !t->provider || !t->provider_instance ||
        !t->provider->ops->unregister_method) {
        return;
    }
    t->provider->ops->unregister_method(t->provider_instance, service, method);
}
