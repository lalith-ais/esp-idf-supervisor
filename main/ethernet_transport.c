/*
 * ethernet_transport.c - Ethernet implementation of network_transport_t
 *
 * This file is the ONLY place that knows about:
 *   - esp_eth_*  (Ethernet driver)
 *   - IP_EVENT_ETH_GOT_IP
 *   - ETH_EVENT_CONNECTED / DISCONNECTED
 *
 * It wraps ethernet_setup.c behind the network_transport_t vtable so
 * network_service.c and everything above it stays transport-agnostic.
 *
 * ethernet_setup.c is UNCHANGED -- all GPIO config, MAC/PHY init,
 * netif glue, and event handler logic lives there exactly as before.
 */

#include "ethernet_transport.h"
#include "ethernet_setup.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "eth-transport";

/* Handles returned by ethernet_init() -- kept here for deinit */
static esp_eth_handle_t *s_eth_handles = NULL;
static uint8_t           s_eth_cnt     = 0;

/* Callbacks supplied by network_service at init time */
static transport_ip_acquired_cb_t  s_on_ip   = NULL;
static transport_disconnected_cb_t s_on_disc = NULL;

/* -------------------------------------------------------------------------
 * Thin shims: ethernet_setup callbacks → network_transport callbacks
 *
 * ethernet_setup.c calls these via the function pointers registered
 * through ethernet_set_ip_callback / ethernet_set_disconnect_callback.
 * We simply forward to whatever network_service installed.
 * ------------------------------------------------------------------------- */
static void eth_ip_shim(void)
{
    if (s_on_ip == NULL) return;

    char ip[16] = {0};
    if (ethernet_get_ip(ip, sizeof(ip)) == ESP_OK) {
        s_on_ip(ip);
    } else {
        ESP_LOGW(TAG, "ip_shim: ethernet_get_ip failed");
    }
}

static void eth_disc_shim(void)
{
    if (s_on_disc != NULL) {
        s_on_disc();
    }
}

/* -------------------------------------------------------------------------
 * vtable implementation
 * ------------------------------------------------------------------------- */
static esp_err_t eth_transport_init(transport_ip_acquired_cb_t  on_ip,
                                    transport_disconnected_cb_t  on_disc)
{
    s_on_ip   = on_ip;
    s_on_disc = on_disc;

    /* Register shims with ethernet_setup BEFORE calling ethernet_init()
     * so we cannot miss an early IP event. */
    ethernet_set_ip_callback(eth_ip_shim);
    ethernet_set_disconnect_callback(eth_disc_shim);

    esp_err_t ret = ethernet_init(&s_eth_handles, &s_eth_cnt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ethernet_init failed: %s", esp_err_to_name(ret));
        s_on_ip   = NULL;
        s_on_disc = NULL;
    }
    return ret;
}

static esp_err_t eth_transport_deinit(void)
{
    esp_err_t ret = ESP_OK;
    if (s_eth_handles != NULL) {
        ret = ethernet_deinit(s_eth_handles, s_eth_cnt);
        s_eth_handles = NULL;
        s_eth_cnt     = 0;
    }
    s_on_ip   = NULL;
    s_on_disc = NULL;
    return ret;
}

static bool eth_transport_is_connected(void)
{
    return ethernet_is_connected();   /* reads eth_connected in ethernet_setup.c */
}

static esp_err_t eth_transport_get_mac(uint8_t mac[6])
{
    return ethernet_get_mac(mac);
}

/* -------------------------------------------------------------------------
 * Public vtable instance
 * ------------------------------------------------------------------------- */
const network_transport_t ethernet_transport = {
    .name         = "ethernet",
    .init         = eth_transport_init,
    .deinit       = eth_transport_deinit,
    .is_connected = eth_transport_is_connected,
    .get_mac      = eth_transport_get_mac,
};
