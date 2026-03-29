/*
 * network_transport.h - Transport driver interface
 *
 * This header defines the ONLY contract between a transport driver
 * (ethernet_transport, wifi_transport, ...) and the network_service layer.
 *
 * A transport driver:
 *   1. Implements init() / deinit() / is_connected() / get_mac()
 *   2. Calls on_ip_acquired() when DHCP delivers an IP
 *   3. Calls on_disconnected() when the link is lost
 *
 * The network_service layer owns all state above this seam.
 * Nothing in mqtt_service, supervisor, or app code includes
 * any transport-specific header.
 */

#ifndef NETWORK_TRANSPORT_H
#define NETWORK_TRANSPORT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Callbacks supplied by network_service to the transport driver.
 * The transport calls these from its own event handler context.
 * Implementations must be ISR/event-task safe (they only post to a queue).
 * ------------------------------------------------------------------------- */
typedef void (*transport_ip_acquired_cb_t)(const char *ip_str);
typedef void (*transport_disconnected_cb_t)(void);

/* -------------------------------------------------------------------------
 * Transport driver vtable.
 *
 * init()         - Start the hardware, register event handlers,
 *                  store the two callbacks for later use.
 *                  Called once by network_service at startup.
 *
 * deinit()       - Stop hardware, unregister handlers, free resources.
 *
 * is_connected() - Return true if the link is up (cable/association).
 *                  Used by the poll-fallback in network_service.
 *
 * get_mac()      - Fill mac[6] with the interface MAC address.
 *                  Used by mqtt_service for node-id derivation.
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;   /* e.g. "ethernet", "wifi" -- for logging only */

    esp_err_t (*init)(transport_ip_acquired_cb_t  on_ip,
                      transport_disconnected_cb_t  on_disc);

    esp_err_t (*deinit)(void);

    bool      (*is_connected)(void);

    esp_err_t (*get_mac)(uint8_t mac[6]);
} network_transport_t;

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_TRANSPORT_H */
