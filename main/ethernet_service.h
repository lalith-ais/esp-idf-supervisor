// ethernet_service.h - FIXED with proper includes
#ifndef ETHERNET_SERVICE_H
#define ETHERNET_SERVICE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ethernet service messages
typedef enum {
    ETH_EVENT_CONNECTED,
    ETH_EVENT_DISCONNECTED,
    ETH_EVENT_GOT_IP,
    ETH_EVENT_STARTED,      // Added
    ETH_EVENT_STOPPED,      // Added
    ETH_EVENT_ERROR
} eth_event_type_t;

typedef struct {
    eth_event_type_t type;
    union {
        struct {
            uint8_t mac[6];
        } connected;
        struct {
            char ip[16];
            char netmask[16];
            char gateway[16];
        } got_ip;
        struct {
            esp_err_t error;
        } error;
    } data;
} eth_service_message_t;

// Public API
void ethernet_service_start(void);
QueueHandle_t ethernet_service_get_queue(void);
bool ethernet_service_is_connected(void);
bool ethernet_service_has_ip(void);
const char* ethernet_service_get_ip(void);
void ethernet_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif // ETHERNET_SERVICE_H