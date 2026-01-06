// system.h - Updated MQTT supervisor section
#ifndef SYSTEM_H
#define SYSTEM_H

#include "supervisor.h"
#include "ethernet_service.h"
#include "mqtt_service.h"
#include "esp_task_wdt.h"

// ------------------------------------------------------------
// Ethernet Supervisor Service
// ------------------------------------------------------------
void ethernet_supervisor_service(void* arg) {
    ESP_LOGI("eth-super", "Ethernet supervisor service starting");
    
    // Start the Ethernet service
    ethernet_service_start();
    
    // Wait for queue
    int max_wait = 50;
    QueueHandle_t eth_queue = NULL;
    
    while (max_wait-- > 0 && eth_queue == NULL) {
        eth_queue = ethernet_service_get_queue();
        if (eth_queue == NULL) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
    
    if (eth_queue == NULL) {
        ESP_LOGE("eth-super", "Failed to get Ethernet queue after waiting");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI("eth-super", "Ethernet queue obtained, monitoring events");
    
    // Register with watchdog if enabled
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
    #endif
    
    while (1) {
        eth_service_message_t msg;
        
        if (xQueueReceive(eth_queue, &msg, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
            // Handle all event types
            switch (msg.type) {
                case ETH_EVENT_CONNECTED:
                    ESP_LOGI("eth-super", "Ethernet connected");
                    break;
                    
                case ETH_EVENT_DISCONNECTED:
                    ESP_LOGW("eth-super", "Ethernet disconnected");
                    break;
                    
                case ETH_EVENT_GOT_IP:
                    ESP_LOGI("eth-super", "Got IP: %s", msg.data.got_ip.ip);
                    break;
                    
                case ETH_EVENT_STARTED:   // Added
                    ESP_LOGI("eth-super", "Ethernet started");
                    break;
                    
                case ETH_EVENT_STOPPED:   // Added
                    ESP_LOGW("eth-super", "Ethernet stopped");
                    break;
                    
                case ETH_EVENT_ERROR:
                    ESP_LOGE("eth-super", "Ethernet hardware error");
                    #ifdef CONFIG_ESP_TASK_WDT
                    esp_task_wdt_delete(NULL);
                    #endif
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    vTaskDelete(NULL);
                    return;
                    
                default:
                    ESP_LOGW("eth-super", "Unknown Ethernet event: %d", msg.type);
                    break;
            }
        }
        
        // Check if Ethernet service is still alive
        if (ethernet_service_get_queue() == NULL) {
            ESP_LOGE("eth-super", "Ethernet service died");
            #ifdef CONFIG_ESP_TASK_WDT
            esp_task_wdt_delete(NULL);
            #endif
            vTaskDelete(NULL);
            return;
        }
        
        // Pet watchdog if enabled
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
        #endif
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    // Clean shutdown
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
    #endif
    vTaskDelete(NULL);
}

// ------------------------------------------------------------
// MQTT Supervisor Service
// ------------------------------------------------------------
void mqtt_supervisor_service(void* arg) {
    ESP_LOGI("mqtt-super", "MQTT supervisor service starting");
    
    mqtt_service_start();
    
    int max_wait = 50;
    QueueHandle_t mqtt_queue = NULL;
    
    while (max_wait-- > 0 && mqtt_queue == NULL) {
        mqtt_queue = mqtt_service_get_queue();
        if (mqtt_queue == NULL) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }
    
    if (mqtt_queue == NULL) {
        ESP_LOGE("mqtt-super", "Failed to get MQTT queue after waiting");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI("mqtt-super", "MQTT queue obtained, monitoring events");
    
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
    #endif
    
    while (1) {
        mqtt_service_message_t msg;
        
        if (xQueueReceive(mqtt_queue, &msg, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
            switch (msg.type) {
                case MQTT_SERVICE_EVENT_CONNECTED:
                    ESP_LOGI("mqtt-super", "MQTT connected");
                    break;
                    
                case MQTT_SERVICE_EVENT_DISCONNECTED:
                    ESP_LOGW("mqtt-super", "MQTT disconnected");
                    break;
                    
                case MQTT_SERVICE_EVENT_MESSAGE_RECEIVED:
                    ESP_LOGI("mqtt-super", "MQTT message: %s -> %s", 
                            msg.data.message.topic, msg.data.message.data);
                    break;
                    
                case MQTT_SERVICE_EVENT_PUBLISHED:
                    ESP_LOGI("mqtt-super", "MQTT published to %s, msg_id=%d",
                            msg.data.published.topic, msg.data.published.msg_id);
                    break;
                    
                case MQTT_SERVICE_EVENT_SUBSCRIBED:
                    ESP_LOGI("mqtt-super", "MQTT subscribed to %s, qos=%d",
                            msg.data.subscribed.topic, msg.data.subscribed.qos);
                    break;
                    
                case MQTT_SERVICE_EVENT_STARTED:
                    ESP_LOGI("mqtt-super", "MQTT service started");
                    break;
                    
                case MQTT_SERVICE_EVENT_STOPPED:
                    ESP_LOGW("mqtt-super", "MQTT service stopped");
                    break;
                    
                case MQTT_SERVICE_EVENT_ERROR:
                    ESP_LOGE("mqtt-super", "MQTT error: %s (code: 0x%x)",
                            msg.data.error.error_msg, msg.data.error.error_code);
                    break;
                    
                default:
                    ESP_LOGW("mqtt-super", "Unknown MQTT event: %d", msg.type);
                    break;
            }
        }
        
        // Check if MQTT service is still alive
        if (mqtt_service_get_queue() == NULL) {
            ESP_LOGE("mqtt-super", "MQTT service died");
            #ifdef CONFIG_ESP_TASK_WDT
            esp_task_wdt_delete(NULL);
            #endif
            vTaskDelete(NULL);
            return;
        }
        
        // Check if Ethernet still has IP
        if (!ethernet_service_has_ip()) {
            ESP_LOGW("mqtt-super", "Ethernet lost IP, MQTT will handle reconnection");
        }
        
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
        #endif
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
    #endif
    vTaskDelete(NULL);
}

// ------------------------------------------------------------
// Service Configuration for Supervisor
// ------------------------------------------------------------
const service_def_t services[] = {
    // Name            Function                    Stack   Prio  Restart         Essential  Context
    {"ethernet",       ethernet_supervisor_service, 12288, 23,   RESTART_ALWAYS,  true,    NULL},
    {"mqtt",           mqtt_supervisor_service,     8192,  20,   RESTART_ALWAYS,  false,   NULL},
    // Add other services as needed
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL}  // Terminator
};

#endif // SYSTEM_H
