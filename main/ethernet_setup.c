#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "ethernet_setup.h"

#define INTERNAL_ETHERNETS_NUM      1

static const char *TAG = "ethernet_setup";
static bool eth_connected = false;
static uint8_t eth_mac_addr[6] = {0};
static char eth_ip_addr[16] = {0};
static ethernet_ip_callback_t ip_callback = NULL; 
static ethernet_disconnect_callback_t disconnect_callback = NULL; 

// Internal function prototypes
static esp_eth_handle_t eth_init_internal(esp_eth_mac_t **mac_out, esp_eth_phy_t **phy_out);
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);


void ethernet_set_disconnect_callback(ethernet_disconnect_callback_t callback)
{
    disconnect_callback = callback;
    ESP_LOGD(TAG, "Disconnect callback set to %p", callback);
}

static esp_eth_handle_t eth_init_internal(esp_eth_mac_t **mac_out, esp_eth_phy_t **phy_out)
{
    esp_eth_handle_t ret = NULL;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;

    // Init common MAC and PHY configs to default
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    // Update PHY config based on board specific configuration
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;
    
    // Init vendor specific MAC config to default
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    
    // Update vendor specific MAC config based on board configuration
    esp32_emac_config.smi_gpio.mdc_num = 31;
    esp32_emac_config.smi_gpio.mdio_num = 52;

    // Create new ESP32 Ethernet MAC instance
    mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Failed to create MAC instance");
        goto err;
    }

    // Create new PHY instance based on board configuration
    phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create PHY instance");
        goto err;
    }

    // Init Ethernet driver to default and install it
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    
    if (esp_eth_driver_install(&config, &eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed");
        goto err;
    }

    if (mac_out != NULL) {
        *mac_out = mac;
    }
    if (phy_out != NULL) {
        *phy_out = phy;
    }
    
    return eth_handle;

err:
    if (mac != NULL) {
        mac->del(mac);
    }
    if (phy != NULL) {
        phy->del(phy);
    }
    return ret;
}

esp_err_t ethernet_init(esp_eth_handle_t **eth_handles_out, uint8_t *eth_cnt_out)
{
    esp_err_t ret = ESP_OK;
    esp_eth_handle_t *eth_handles = NULL;
    uint8_t eth_cnt = 0;

    if (eth_handles_out == NULL || eth_cnt_out == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    eth_handles = calloc(INTERNAL_ETHERNETS_NUM, sizeof(esp_eth_handle_t));
    if (eth_handles == NULL) {
        ESP_LOGE(TAG, "No memory for Ethernet handles");
        return ESP_ERR_NO_MEM;
    }

    // Initialize internal Ethernet
    eth_handles[eth_cnt] = eth_init_internal(NULL, NULL);
    if (eth_handles[eth_cnt] == NULL) {
        ESP_LOGE(TAG, "Internal Ethernet init failed");
        free(eth_handles);
        return ESP_FAIL;
    }
    eth_cnt++;

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, 
                                               &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, 
                                               &got_ip_event_handler, NULL));

    // Create network interface
    esp_netif_t *eth_netif = esp_netif_new(&(esp_netif_config_t)ESP_NETIF_DEFAULT_ETH());
    esp_eth_netif_glue_handle_t eth_netif_glue = esp_eth_new_netif_glue(eth_handles[0]);
    
    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, eth_netif_glue));

    // Start Ethernet driver
    for (int i = 0; i < eth_cnt; i++) {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
    }

    *eth_handles_out = eth_handles;
    *eth_cnt_out = eth_cnt;

    ESP_LOGI(TAG, "Ethernet initialized successfully");
    return ret;
}

esp_err_t ethernet_deinit(esp_eth_handle_t *eth_handles, uint8_t eth_cnt)
{
    if (eth_handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < eth_cnt; i++) {
        esp_eth_mac_t *mac = NULL;
        esp_eth_phy_t *phy = NULL;
        
        if (eth_handles[i] != NULL) {
            esp_eth_get_mac_instance(eth_handles[i], &mac);
            esp_eth_get_phy_instance(eth_handles[i], &phy);
            esp_eth_driver_uninstall(eth_handles[i]);
        }
        
        if (mac != NULL) {
            mac->del(mac);
        }
        if (phy != NULL) {
            phy->del(phy);
        }
    }

    free(eth_handles);
    
    // Unregister event handlers
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler);
    
    ESP_LOGI(TAG, "Ethernet deinitialized");
    return ESP_OK;
}

bool ethernet_is_connected(void)
{
    return eth_connected;
}

esp_err_t ethernet_get_mac(uint8_t *mac_addr)
{
    if (mac_addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(mac_addr, eth_mac_addr, 6);
    return ESP_OK;
}

esp_err_t ethernet_get_ip(char *ip_addr, size_t len)
{
    if (ip_addr == NULL || len < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(ip_addr, eth_ip_addr, len - 1);
    ip_addr[len - 1] = '\0';
    return ESP_OK;
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, 
                              int32_t event_id, void *event_data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, eth_mac_addr);
        eth_connected = true;
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 eth_mac_addr[0], eth_mac_addr[1], eth_mac_addr[2],
                 eth_mac_addr[3], eth_mac_addr[4], eth_mac_addr[5]);
        break;
        
    case ETHERNET_EVENT_DISCONNECTED:
        eth_connected = false;
        ESP_LOGI(TAG, "Ethernet Link Down");
        if (disconnect_callback != NULL) {
            ESP_LOGD(TAG, "Calling disconnect callback");
            disconnect_callback();
        }
        break;
        
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
        
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
        
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, 
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    snprintf(eth_ip_addr, sizeof(eth_ip_addr), IPSTR, IP2STR(&ip_info->ip));
    
    ESP_LOGI(TAG, "Got IP Address");
    ESP_LOGI(TAG, "IP:      " IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info->gw));
    
    if (ip_callback != NULL) {
        ESP_LOGD(TAG, "Calling IP callback");
        ip_callback();
    }
}

void ethernet_set_ip_callback(ethernet_ip_callback_t callback)
{
    ip_callback = callback;
    ESP_LOGD(TAG, "IP callback set to %p", callback);
}
