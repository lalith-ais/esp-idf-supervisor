// ethernet_service.c - COMPLETE FIXED VERSION
#include "ethernet_service.h"
#include "ethernet_setup.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <string.h>

static const char *TAG = "eth-service";

// Service context
typedef struct {
    QueueHandle_t event_queue;
    bool is_running;
    bool is_connected;
    bool has_ip;
    esp_eth_handle_t* eth_handles;
    uint8_t eth_cnt;
    TaskHandle_t task_handle;
} eth_service_context_t;

static eth_service_context_t eth_ctx = {0};

// Convert ESP-IDF callbacks to service messages
static void eth_ip_obtained_callback(void)
{
    ESP_LOGI(TAG, "=== IP OBTAINED CALLBACK FIRED ===");
    
    eth_service_message_t msg = {
        .type = ETH_EVENT_GOT_IP,
    };
    
    // Get IP info
    char ip_addr[16];
    ethernet_get_ip(ip_addr, sizeof(ip_addr));
    strncpy(msg.data.got_ip.ip, ip_addr, sizeof(msg.data.got_ip.ip) - 1);
    ESP_LOGI(TAG, "IP from ethernet_get_ip: %s", ip_addr);
    
    // Send to service queue
    if (eth_ctx.event_queue != NULL) {
        BaseType_t result = xQueueSend(eth_ctx.event_queue, &msg, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "Queue send result: %d", result);
    } else {
        ESP_LOGE(TAG, "Event queue is NULL!");
    }
}

static void eth_disconnect_callback(void)
{
    ESP_LOGI(TAG, "=== DISCONNECT CALLBACK FIRED ===");
    
    eth_service_message_t msg = {
        .type = ETH_EVENT_DISCONNECTED,
    };
    
    if (eth_ctx.event_queue != NULL) {
        xQueueSend(eth_ctx.event_queue, &msg, pdMS_TO_TICKS(100));
    }
}

// Ethernet service main function
static void ethernet_service_task(void* arg)
{
    ESP_LOGI(TAG, "Ethernet service starting");
    
    // Register with watchdog if enabled
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
    #endif
    
    // Initialize the service context
    eth_ctx.event_queue = xQueueCreate(10, sizeof(eth_service_message_t));
    eth_ctx.is_running = true;
    eth_ctx.task_handle = xTaskGetCurrentTaskHandle();
    eth_ctx.is_connected = false;
    eth_ctx.has_ip = false;
    
    if (eth_ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
        #endif
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Setting up Ethernet callbacks...");
    // Set up ESP-IDF callbacks that forward to our queue
    ethernet_set_ip_callback(eth_ip_obtained_callback);
    ethernet_set_disconnect_callback(eth_disconnect_callback);
    ESP_LOGI(TAG, "Callbacks set up");
    
    // Initialize Ethernet hardware (using your existing code)
    ESP_LOGI(TAG, "Initializing Ethernet hardware...");
    esp_err_t ret = ethernet_init(&eth_ctx.eth_handles, &eth_ctx.eth_cnt);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet hardware initialization failed");
        
        // Send error message
        eth_service_message_t error_msg = {
            .type = ETH_EVENT_ERROR,
            .data.error.error = ret
        };
        xQueueSend(eth_ctx.event_queue, &error_msg, 0);
        
        // Clean up queue before crashing
        vQueueDelete(eth_ctx.event_queue);
        eth_ctx.event_queue = NULL;
        
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
        #endif
        
        // Crash service - supervisor will restart with backoff
        vTaskDelay(pdMS_TO_TICKS(1000));
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Ethernet service running, waiting for events...");
    
    // Main service loop
    char last_ip[16] = {0};
    while (eth_ctx.is_running) {
        eth_service_message_t msg;
        
        // Check for events from ESP-IDF callbacks (forwarded via queue)
        if (xQueueReceive(eth_ctx.event_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Handle the event
            switch (msg.type) {
                case ETH_EVENT_GOT_IP:
                    ESP_LOGI(TAG, "Service: Got IP %s", msg.data.got_ip.ip);
                    eth_ctx.is_connected = true;
                    eth_ctx.has_ip = true;
                    strncpy(last_ip, msg.data.got_ip.ip, sizeof(last_ip) - 1);
                    break;
                    
                case ETH_EVENT_DISCONNECTED:
                    ESP_LOGI(TAG, "Service: Ethernet disconnected");
                    eth_ctx.is_connected = false;
                    eth_ctx.has_ip = false;
                    memset(last_ip, 0, sizeof(last_ip));
                    break;
                
                case ETH_EVENT_STARTED:    // New
					ESP_LOGI(TAG, "Service: Ethernet started");
					break;
        
				case ETH_EVENT_STOPPED:    // New
					ESP_LOGI(TAG, "Service: Ethernet stopped");
					eth_ctx.is_connected = false;
					eth_ctx.has_ip = false;
					break;
                    
                case ETH_EVENT_ERROR:
                    ESP_LOGE(TAG, "Service: Ethernet error %s", 
                            esp_err_to_name(msg.data.error.error));
                    eth_ctx.is_running = false;
                    break;
                    
                default:
                    break;
            }
        }
        
        // Poll for connection status
        bool hardware_connected = ethernet_is_connected();
        if (hardware_connected != eth_ctx.is_connected) {
            eth_ctx.is_connected = hardware_connected;
            
            eth_service_message_t status_msg = {
                .type = hardware_connected ? ETH_EVENT_CONNECTED : ETH_EVENT_DISCONNECTED
            };
            
            if (hardware_connected) {
                uint8_t mac[6];
                ethernet_get_mac(mac);
                memcpy(status_msg.data.connected.mac, mac, 6);
                ESP_LOGI(TAG, "Poll: Ethernet connected, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            } else {
                ESP_LOGI(TAG, "Poll: Ethernet disconnected");
                eth_ctx.has_ip = false;
                memset(last_ip, 0, sizeof(last_ip));
            }
            
            xQueueSend(eth_ctx.event_queue, &status_msg, 0);
        }
        
        // Poll for IP address (backup in case callback doesn't fire)
        if (eth_ctx.is_connected && !eth_ctx.has_ip) {
            char current_ip[16] = {0};
            ethernet_get_ip(current_ip, sizeof(current_ip));
            
            if (strlen(current_ip) > 0 && strcmp(current_ip, "0.0.0.0") != 0) {
                ESP_LOGI(TAG, "Polling detected IP: %s", current_ip);
                eth_ctx.has_ip = true;
                strncpy(last_ip, current_ip, sizeof(last_ip) - 1);
                
                eth_service_message_t ip_msg = {
                    .type = ETH_EVENT_GOT_IP,
                };
                strncpy(ip_msg.data.got_ip.ip, current_ip, sizeof(ip_msg.data.got_ip.ip) - 1);
                xQueueSend(eth_ctx.event_queue, &ip_msg, 0);
            }
        }
        
        // Check if we need to exit
        if (!eth_ctx.is_running) {
            break;
        }
        
        // Pet the watchdog
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
        #endif
        
        // Small delay
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Cleanup
    ESP_LOGI(TAG, "Ethernet service cleaning up...");
    if (eth_ctx.eth_handles != NULL) {
        ethernet_deinit(eth_ctx.eth_handles, eth_ctx.eth_cnt);
        eth_ctx.eth_handles = NULL;
    }
    
    if (eth_ctx.event_queue != NULL) {
        vQueueDelete(eth_ctx.event_queue);
        eth_ctx.event_queue = NULL;
    }
    
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
    #endif
    
    vTaskDelete(NULL);
}

// Public API implementation
void ethernet_service_start(void)
{
    // Only start if not already running
    if (eth_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "Ethernet service already running");
        return;
    }
    
    // Create the Ethernet service task
    xTaskCreate(ethernet_service_task,
                "eth-service",
                12288,
                NULL,
                22,
                &eth_ctx.task_handle);
}

QueueHandle_t ethernet_service_get_queue(void)
{
    return eth_ctx.event_queue;
}

bool ethernet_service_is_connected(void)
{
    return eth_ctx.is_connected;
}

bool ethernet_service_has_ip(void)
{
    return eth_ctx.has_ip;
}

const char* ethernet_service_get_ip(void)
{
    static char ip[16] = {0};
    if (eth_ctx.has_ip) {
        ethernet_get_ip(ip, sizeof(ip));
    }
    return ip;
}

void ethernet_service_stop(void)
{
    if (eth_ctx.task_handle != NULL) {
        eth_ctx.is_running = false;
        
        // Give task time to clean up
        vTaskDelay(500 / portTICK_PERIOD_MS);
        
        // Force delete if still running
        if (eTaskGetState(eth_ctx.task_handle) != eDeleted) {
            vTaskDelete(eth_ctx.task_handle);
        }
        
        eth_ctx.task_handle = NULL;
    }
}
