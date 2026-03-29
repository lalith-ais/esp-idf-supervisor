/*
 * ethernet_transport.h - Ethernet implementation of network_transport_t
 *
 * system.c includes this to get the &ethernet_transport vtable pointer
 * and passes it to network_service_start().
 *
 * Nothing above system.c should include this header.
 */

#ifndef ETHERNET_TRANSPORT_H
#define ETHERNET_TRANSPORT_H

#include "network_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The single vtable instance -- defined in ethernet_transport.c */
extern const network_transport_t ethernet_transport;

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_TRANSPORT_H */
