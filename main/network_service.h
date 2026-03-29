/*
 * network_service.h - Transport-agnostic network state service
 *
 * This is the ONLY networking header that mqtt_service, supervisor,
 * and application code should include.  It deliberately exposes no
 * transport concepts (no esp_eth_*, no esp_wifi_*, no netif keys).
 *
 * Queue message types use NET_EVENT_* names so they carry no transport
 * identity.  The same binary message stream works for Ethernet today
 * and WiFi tomorrow without touching any consumer code.
 */

#ifndef NETWORK_SERVICE_H
#define NETWORK_SERVICE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "network_transport.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Queue message type -- transport-agnostic
 * ------------------------------------------------------------------------- */
typedef enum {
    NET_EVENT_CONNECTED,        /* link up (cable / association) */
    NET_EVENT_DISCONNECTED,     /* link lost */
    NET_EVENT_GOT_IP,           /* DHCP assigned an IP */
    NET_EVENT_STARTED,          /* service started */
    NET_EVENT_STOPPED,          /* service stopped */
    NET_EVENT_ERROR,            /* unrecoverable hardware error */
    NET_EVENT_STOP_REQUESTED    /* sent by network_service_stop() */
} net_event_type_t;

typedef struct {
    net_event_type_t type;
    union {
        struct { uint8_t mac[6]; }           connected;
        struct { char    ip[16];
                 char    netmask[16];
                 char    gateway[16]; }      got_ip;
        struct { esp_err_t error; }          error;
    } data;
} net_service_message_t;

/* -------------------------------------------------------------------------
 * Lifecycle  (system.c passes the chosen transport vtable)
 * ------------------------------------------------------------------------- */
void network_service_start(const network_transport_t *transport);
void network_service_stop(void);

/* -------------------------------------------------------------------------
 * State queries (used by mqtt_service and supervisor)
 * ------------------------------------------------------------------------- */
QueueHandle_t  network_service_get_queue(void);
bool           network_service_is_connected(void);
bool           network_service_has_ip(void);
const char    *network_service_get_ip(void);

/* -------------------------------------------------------------------------
 * MAC address (used by mqtt_service for node-id derivation)
 * Delegates to transport->get_mac(); returns ESP_ERR_INVALID_STATE
 * if called before the transport is initialised.
 * ------------------------------------------------------------------------- */
esp_err_t network_service_get_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_SERVICE_H */
