#ifndef ETHERNET_SETUP_H
#define ETHERNET_SETUP_H

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ethernet_ip_callback_t)(void);
void ethernet_set_ip_callback(ethernet_ip_callback_t callback);
typedef void (*ethernet_disconnect_callback_t)(void); 
void ethernet_set_disconnect_callback(ethernet_disconnect_callback_t callback);


/**
 * @brief Initialize Ethernet driver
 * 
 * @param[out] eth_handles_out Array of Ethernet handles
 * @param[out] eth_cnt_out Number of initialized Ethernet interfaces
 * @return esp_err_t 
 */
esp_err_t ethernet_init(esp_eth_handle_t **eth_handles_out, uint8_t *eth_cnt_out);

/**
 * @brief Deinitialize Ethernet driver
 * 
 * @param eth_handles Array of Ethernet handles
 * @param eth_cnt Number of Ethernet interfaces
 * @return esp_err_t 
 */
esp_err_t ethernet_deinit(esp_eth_handle_t *eth_handles, uint8_t eth_cnt);

/**
 * @brief Get Ethernet connection status
 * 
 * @return true if Ethernet is connected
 * @return false if Ethernet is disconnected
 */
bool ethernet_is_connected(void);

/**
 * @brief Get the MAC address
 * 
 * @param mac_addr Buffer to store MAC address (6 bytes)
 * @return esp_err_t 
 */
esp_err_t ethernet_get_mac(uint8_t *mac_addr);

/**
 * @brief Get the IP address
 * 
 * @param ip_addr Buffer to store IP address string
 * @param len Length of buffer
 * @return esp_err_t 
 */
esp_err_t ethernet_get_ip(char *ip_addr, size_t len);

#ifdef __cplusplus
}
#endif

#endif // ETHERNET_SETUP_H
