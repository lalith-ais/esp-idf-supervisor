// system.h - CORRECTED VERSION
#ifndef SYSTEM_H
#define SYSTEM_H

#include "supervisor.h"
#include "ethernet_service.h"
#include "mqtt_service.h"
#include "ds18b20_temp.h"
#include "esp_task_wdt.h"


// ============================================================
// SUPERVISOR TEMPLATE MACROS
// ============================================================

// Generic supervisor template - UPDATED with _service_ prefix
#define DEFINE_SERVICE_SUPERVISOR(service_name, check_func, queue_func) \
void service_name##_supervisor(void* arg) { \
    ESP_LOGI(#service_name"-super", #service_name" supervisor starting"); \
    service_name##_service_start(); \  /* CHANGED: Added _service_ */ \
    \
    int max_wait = 50; \
    QueueHandle_t queue = NULL; \
    while (max_wait-- > 0 && queue == NULL) { \
        queue = queue_func(); \
        if (queue == NULL) vTaskDelay(10 / portTICK_PERIOD_MS); \
    } \
    \
    if (queue == NULL) { \
        ESP_LOGE(#service_name"-super", "Failed to get queue"); \
        vTaskDelete(NULL); \
        return; \
    } \
    \
    ESP_LOGI(#service_name"-super", #service_name" supervisor running"); \
    while (1) { \
        /* Generic supervisor logic */ \
        if (check_func() == false) { \
            ESP_LOGW(#service_name"-super", "Service health check failed"); \
        } \
        vTaskDelay(1000 / portTICK_PERIOD_MS); \
    } \
    vTaskDelete(NULL); \
}

// ============================================================
// SERVICE-SPECIFIC SUPERVISORS (Using template)
// ============================================================

// Ethernet supervisor (using template) - UPDATED function names
void ethernet_supervisor(void* arg) {
    ESP_LOGI("ethernet-super", "Ethernet supervisor starting");
    ethernet_service_start();  /* CORRECT function name */
    
    int max_wait = 50;
    QueueHandle_t queue = NULL;
    while (max_wait-- > 0 && queue == NULL) {
        queue = ethernet_service_get_queue();
        if (queue == NULL) vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    
    if (queue == NULL) {
        ESP_LOGE("ethernet-super", "Failed to get Ethernet queue");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI("ethernet-super", "Ethernet supervisor running");
    
    // Register with watchdog if enabled
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
    #endif
    
    while (1) {
        // Check queue for events
        eth_service_message_t msg;
        if (xQueueReceive(queue, &msg, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
            switch (msg.type) {
                case ETH_EVENT_CONNECTED:
                    ESP_LOGI("ethernet-super", "Ethernet connected");
                    break;
                case ETH_EVENT_DISCONNECTED:
                    ESP_LOGW("ethernet-super", "Ethernet disconnected");
                    break;
                case ETH_EVENT_GOT_IP:
                    ESP_LOGI("ethernet-super", "Got IP: %s", msg.data.got_ip.ip);
                    break;
                case ETH_EVENT_STARTED:
                    ESP_LOGI("ethernet-super", "Ethernet started");
                    break;
                case ETH_EVENT_STOPPED:
                    ESP_LOGW("ethernet-super", "Ethernet stopped");
                    break;
                case ETH_EVENT_ERROR:
                    ESP_LOGE("ethernet-super", "Ethernet hardware error");
                    #ifdef CONFIG_ESP_TASK_WDT
                    esp_task_wdt_delete(NULL);
                    #endif
                    vTaskDelete(NULL);
                    return;
                default:
                    ESP_LOGW("ethernet-super", "Unknown Ethernet event: %d", msg.type);
                    break;
            }
        }
        
        // Pet watchdog if enabled
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

// MQTT supervisor (using template) - UPDATED function names  
void mqtt_supervisor(void* arg) {
    ESP_LOGI("mqtt-super", "MQTT supervisor starting");
    mqtt_service_start();  /* CORRECT function name */
    
    int max_wait = 50;
    QueueHandle_t queue = NULL;
    while (max_wait-- > 0 && queue == NULL) {
        queue = mqtt_service_get_queue();
        if (queue == NULL) vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    
    if (queue == NULL) {
        ESP_LOGE("mqtt-super", "Failed to get MQTT queue");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI("mqtt-super", "MQTT supervisor running");
    
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
    #endif
    
    while (1) {
        mqtt_service_message_t msg;
        
        if (xQueueReceive(queue, &msg, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
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

// ds18b20 temerature servoce
void ds18b20_temp_supervisor(void* arg) {
    ESP_LOGI("ds18b20temp-super", "ds18b20 temperature supervisor starting");
    ds18b20_temp_service_start();
    
    int max_wait = 50;
    QueueHandle_t queue = NULL;
    while (max_wait-- > 0 && queue == NULL) {
        queue = ds18b20_temp_service_get_queue();
        if (queue == NULL) vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    
    if (queue == NULL) {
        ESP_LOGE("ds18b20-temp-super", "Failed to get ds18b20 temperature queue");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI("ds18b20-temp-super", "ds18b20 temperature supervisor running");
    
    // Register with watchdog if enabled
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
    #endif
    
    uint32_t last_message_count = 0;
    uint32_t stale_count = 0;
    
    while (1) {
        // Monitor temperature readings from queue
        float temperature;
        if (xQueueReceive(queue, &temperature, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
            ESP_LOGI("18b20-temp-super", "Monitor: %.2f°C", temperature);
            
            // Reset stale counter on new data
            stale_count = 0;
        } else {
            // No data in queue - check if service is still publishing
            uint32_t current_count = ds18b20_temp_service_get_message_count();  // USE PUBLIC API
            if (current_count == last_message_count) {
                stale_count++;
                if (stale_count > 10) {  // 10 seconds without new data
                    ESP_LOGW("ds18b20-temp-super", "No new temperature data for %d seconds", 
                            stale_count);
                }
            }
            last_message_count = current_count;
        }
        
        // Check service health
        if (!ds18b20_temp_service_is_healthy()) {
            ESP_LOGW("ds18b20-temp-super", "ds18b20 temperature service health check failed");
        }
        
        // Pet watchdog if enabled
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
// ============================================================
// SERVICE REGISTRY
// ============================================================

const service_def_t services[] = {
    // Core services
    {"ethernet",   ethernet_supervisor, 12288, 23, RESTART_ALWAYS, true,  NULL},
    {"mqtt",       mqtt_supervisor,     8192,  20, RESTART_ALWAYS, false, NULL},
    {"ds18b20-temp", ds18b20_temp_supervisor, 4096, 10, RESTART_ALWAYS, false, NULL},
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL}
};

#endif // SYSTEM_H
