/*
 * ethernet_service.h  (v1.2 -- hardened)
 *
 * CHANGE vs original:
 *  - ETH_EVENT_STOP_REQUESTED added for clean queue-based shutdown [4]
 */

#ifndef ETHERNET_SERVICE_H
#define ETHERNET_SERVICE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ETH_EVENT_CONNECTED,
    ETH_EVENT_DISCONNECTED,
    ETH_EVENT_GOT_IP,
    ETH_EVENT_STARTED,
    ETH_EVENT_STOPPED,
    ETH_EVENT_ERROR,
    ETH_EVENT_STOP_REQUESTED   /* sent by ethernet_service_stop() for clean shutdown */
} eth_event_type_t;

typedef struct {
    eth_event_type_t type;
    union {
        struct { uint8_t mac[6]; }       connected;
        struct { char ip[16];
                 char netmask[16];
                 char gateway[16]; }     got_ip;
        struct { esp_err_t error; }      error;
    } data;
} eth_service_message_t;

void           ethernet_service_start(void);
void           ethernet_service_stop(void);
QueueHandle_t  ethernet_service_get_queue(void);
bool           ethernet_service_is_connected(void);
bool           ethernet_service_has_ip(void);
const char    *ethernet_service_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_SERVICE_H */
