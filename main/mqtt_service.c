#include "mqtt_service.h"
#include "app_mqtt.h"
#include "ethernet_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <string.h>

static const char *TAG = "mqtt-service";

// Service context
typedef struct {
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    TaskHandle_t publish_task_handle;
    bool is_running;
    bool is_connected;
    bool publish_task_running;
    mqtt_config_t config;
    uint32_t message_counter;
} mqtt_service_context_t;

static mqtt_service_context_t mqtt_ctx = {0};

// Callback forward declarations
static void mqtt_message_callback(const char *topic, const char *data, void *ctx);
static void mqtt_connection_callback(bool connected, void *ctx);

// Publish task function
static void mqtt_publish_task(void* arg)
{
    ESP_LOGI(TAG, "MQTT publish task started");
    
    char message[256];
    uint32_t counter = 0;
    
    while (mqtt_ctx.publish_task_running) {
        // Wait for MQTT to be connected
        if (!mqtt_ctx.is_connected || !mqtt_service_is_running()) {
            ESP_LOGD(TAG, "MQTT not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Check if Ethernet still has IP
        if (!ethernet_service_has_ip()) {
            ESP_LOGW(TAG, "No Ethernet IP, waiting...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        // Create and publish message
        snprintf(message, sizeof(message), 
                "Counter: %lu, Free Heap: %"PRIu32, 
                counter++, esp_get_free_heap_size());
        
        int msg_id = mqtt_client_publish(mqtt_ctx.config.publish_topic, 
                                        message, 0, 0, 0);
        
        if (msg_id >= 0) {
            // Send published event
            mqtt_service_message_t pub_msg = {
                .type = MQTT_SERVICE_EVENT_PUBLISHED,
                .data.published.msg_id = msg_id
            };
            strncpy(pub_msg.data.published.topic, 
                   mqtt_ctx.config.publish_topic,
                   sizeof(pub_msg.data.published.topic) - 1);
            
            if (mqtt_ctx.event_queue != NULL) {
                xQueueSend(mqtt_ctx.event_queue, &pub_msg, 0);
            }
            
            ESP_LOGI(TAG, "Published to %s: %s", 
                    mqtt_ctx.config.publish_topic, message);
        } else {
            ESP_LOGW(TAG, "Failed to publish message");
        }
        
        vTaskDelay(pdMS_TO_TICKS(mqtt_ctx.config.publish_interval_ms));
    }
    
    ESP_LOGI(TAG, "MQTT publish task stopping");
    vTaskDelete(NULL);
}


// MQTT service main task
static void mqtt_service_task(void* arg)
{
    ESP_LOGI(TAG, "MQTT service starting");
    
    // Register with watchdog
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
    #endif
    
    // Initialize context
    mqtt_ctx.event_queue = xQueueCreate(20, sizeof(mqtt_service_message_t));
    mqtt_ctx.is_running = true;
    mqtt_ctx.task_handle = xTaskGetCurrentTaskHandle();
    mqtt_ctx.is_connected = false;
    mqtt_ctx.publish_task_running = false;
    mqtt_ctx.message_counter = 0;
    
    if (mqtt_ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
        #endif
        vTaskDelete(NULL);
        return;
    }
    
    // Default configuration if not set
    if (strlen(mqtt_ctx.config.broker_uri) == 0) {
        strcpy(mqtt_ctx.config.broker_uri, "mqtt://192.168.124.4");
        strcpy(mqtt_ctx.config.client_id, "ESP32P4-ETH");
        strcpy(mqtt_ctx.config.publish_topic, "/ESP32P4/NODE1");
        strcpy(mqtt_ctx.config.subscribe_topic, "/ESP32P4/COMMAND");
        mqtt_ctx.config.enabled = true;
        mqtt_ctx.config.publish_interval_ms = 5000;
    }
    
    // Wait for Ethernet to have IP
    int wait_count = 0;
    while (!ethernet_service_has_ip()) {
        ESP_LOGI(TAG, "Waiting for Ethernet IP... (%d)", ++wait_count);
        
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
        #endif
        
        if (wait_count > 60) {  // 2 minutes timeout
            ESP_LOGE(TAG, "Ethernet IP timeout, crashing service");
            mqtt_ctx.is_running = false;
            
            mqtt_service_message_t error_msg = {
                .type = MQTT_SERVICE_EVENT_ERROR,
                .data.error.error_code = ESP_ERR_TIMEOUT
            };
            strcpy(error_msg.data.error.error_msg, "Ethernet IP timeout");
            
            xQueueSend(mqtt_ctx.event_queue, &error_msg, 0);
            
            #ifdef CONFIG_ESP_TASK_WDT
            esp_task_wdt_delete(NULL);
            #endif
            
            vQueueDelete(mqtt_ctx.event_queue);
            mqtt_ctx.event_queue = NULL;
            
            vTaskDelete(NULL);
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    // Initialize MQTT client
    ESP_LOGI(TAG, "Initializing MQTT client: %s", mqtt_ctx.config.broker_uri);
    esp_err_t ret = mqtt_client_init(mqtt_ctx.config.broker_uri, 
                                     mqtt_ctx.config.client_id);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        mqtt_ctx.is_running = false;
        
        mqtt_service_message_t error_msg = {
            .type = MQTT_SERVICE_EVENT_ERROR,
            .data.error.error_code = ret
        };
        strcpy(error_msg.data.error.error_msg, "MQTT init failed");
        
        xQueueSend(mqtt_ctx.event_queue, &error_msg, 0);
        
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
        #endif
        
        vQueueDelete(mqtt_ctx.event_queue);
        mqtt_ctx.event_queue = NULL;
        
        vTaskDelete(NULL);
        return;
    }
    
    // Set callbacks
    mqtt_client_set_message_callback(mqtt_message_callback, &mqtt_ctx);
    mqtt_client_set_connection_callback(mqtt_connection_callback, &mqtt_ctx);
    
    // Start MQTT client
    ESP_LOGI(TAG, "Starting MQTT client");
    ret = mqtt_client_start();
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        mqtt_client_deinit();
        mqtt_ctx.is_running = false;
        
        mqtt_service_message_t error_msg = {
            .type = MQTT_SERVICE_EVENT_ERROR,
            .data.error.error_code = ret
        };
        strcpy(error_msg.data.error.error_msg, "MQTT start failed");
        
        xQueueSend(mqtt_ctx.event_queue, &error_msg, 0);
        
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
        #endif
        
        vQueueDelete(mqtt_ctx.event_queue);
        mqtt_ctx.event_queue = NULL;
        
        vTaskDelete(NULL);
        return;
    }
    
    // Send started event
    mqtt_service_message_t started_msg = {
        .type = MQTT_SERVICE_EVENT_STARTED
    };
    xQueueSend(mqtt_ctx.event_queue, &started_msg, 0);
    
    // Start publish task
    mqtt_ctx.publish_task_running = true;
    xTaskCreate(mqtt_publish_task,
                "mqtt-publish",
                4096,
                NULL,
                5,
                &mqtt_ctx.publish_task_handle);
    
    ESP_LOGI(TAG, "MQTT service running");
    
    // Main service loop
    while (mqtt_ctx.is_running) {
        // Check if Ethernet still has IP
        if (!ethernet_service_has_ip()) {
            ESP_LOGW(TAG, "Lost Ethernet IP, stopping MQTT...");
            mqtt_client_stop();
            mqtt_ctx.is_connected = false;
            
            // Wait for reconnection
            int reconnect_wait = 0;
            while (!ethernet_service_has_ip() && mqtt_ctx.is_running) {
                #ifdef CONFIG_ESP_TASK_WDT
                esp_task_wdt_reset();
                #endif
                
                vTaskDelay(pdMS_TO_TICKS(1000));
                reconnect_wait++;
                
                if (reconnect_wait > 30) {
                    ESP_LOGE(TAG, "Ethernet reconnection timeout");
                    mqtt_ctx.is_running = false;
                    break;
                }
            }
            
            if (mqtt_ctx.is_running && ethernet_service_has_ip()) {
                ESP_LOGI(TAG, "Ethernet restored, restarting MQTT");
                mqtt_client_start();
            }
        }
        
        // Pet watchdog
        #ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
        #endif
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Clean shutdown
    ESP_LOGI(TAG, "MQTT service cleaning up...");
    
    // Stop publish task
    mqtt_ctx.publish_task_running = false;
    if (mqtt_ctx.publish_task_handle != NULL) {
        // Give task time to clean up
        vTaskDelay(pdMS_TO_TICKS(200));
        mqtt_ctx.publish_task_handle = NULL;
    }
    
    // Stop MQTT client
    mqtt_client_deinit();
    
    // Send stopped event
    mqtt_service_message_t stopped_msg = {
        .type = MQTT_SERVICE_EVENT_STOPPED
    };
    xQueueSend(mqtt_ctx.event_queue, &stopped_msg, 0);
    
    // Clean up queue
    if (mqtt_ctx.event_queue != NULL) {
        vQueueDelete(mqtt_ctx.event_queue);
        mqtt_ctx.event_queue = NULL;
    }
    
    #ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
    #endif
    
    ESP_LOGI(TAG, "MQTT service stopped");
    vTaskDelete(NULL);
}

// MQTT message callback
static void mqtt_message_callback(const char *topic, const char *data, void *ctx)
{
    ESP_LOGI(TAG, "Message received - Topic: %s, Data: %s", topic, data);
    
    // Process commands
    if (strcmp(topic, mqtt_ctx.config.subscribe_topic) == 0) {
        if (strcmp(data, "led_on") == 0) {
            ESP_LOGI(TAG, "Turning LED ON");
        } else if (strcmp(data, "led_off") == 0) {
            ESP_LOGI(TAG, "Turning LED OFF");
        } else if (strcmp(data, "reboot") == 0) {
            ESP_LOGI(TAG, "Reboot command received");
            // You could trigger a reboot through supervisor
        }
    }
    
    // Forward to service queue
    mqtt_service_message_t msg = {
        .type = MQTT_SERVICE_EVENT_MESSAGE_RECEIVED
    };
    strncpy(msg.data.message.topic, topic, sizeof(msg.data.message.topic) - 1);
    strncpy(msg.data.message.data, data, sizeof(msg.data.message.data) - 1);
    
    if (mqtt_ctx.event_queue != NULL) {
        xQueueSend(mqtt_ctx.event_queue, &msg, 0);
    }
    
    mqtt_ctx.message_counter++;
}

// MQTT connection callback
static void mqtt_connection_callback(bool connected, void *ctx)
{
    mqtt_ctx.is_connected = connected;
    
    mqtt_service_message_t msg = {
        .type = connected ? MQTT_SERVICE_EVENT_CONNECTED : MQTT_SERVICE_EVENT_DISCONNECTED
    };
    
    if (connected) {
        ESP_LOGI(TAG, "MQTT connected");
        // Subscribe to topic
        mqtt_client_subscribe(mqtt_ctx.config.subscribe_topic, 0);
        // Send connection announcement
        mqtt_client_publish(mqtt_ctx.config.publish_topic, 
                           "ESP32-P4 MQTT connected!", 0, 1, 0);
    } else {
        ESP_LOGI(TAG, "MQTT disconnected");
    }
    
    if (mqtt_ctx.event_queue != NULL) {
        xQueueSend(mqtt_ctx.event_queue, &msg, 0);
    }
}

// Public API implementation

void mqtt_service_start(void)
{
    if (mqtt_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "MQTT service already running");
        return;
    }
    
    xTaskCreate(mqtt_service_task,
                "mqtt-service",
                8192,
                NULL,
                19,
                &mqtt_ctx.task_handle);
}

void mqtt_service_stop(void)
{
    mqtt_ctx.is_running = false;
    
    if (mqtt_ctx.task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
        mqtt_ctx.task_handle = NULL;
    }
}

QueueHandle_t mqtt_service_get_queue(void)
{
    return mqtt_ctx.event_queue;
}

bool mqtt_service_is_connected(void)
{
    return mqtt_ctx.is_connected;
}

bool mqtt_service_is_running(void)
{
    return mqtt_ctx.is_running;
}

void mqtt_service_set_config(const mqtt_config_t *config)
{
    if (config == NULL) return;
    
    memcpy(&mqtt_ctx.config, config, sizeof(mqtt_config_t));
}

void mqtt_service_get_config(mqtt_config_t *config)
{
    if (config == NULL) return;
    
    memcpy(config, &mqtt_ctx.config, sizeof(mqtt_config_t));
}

esp_err_t mqtt_service_publish(const char *topic, const char *data, int qos, bool retain)
{
    if (!mqtt_ctx.is_connected || !mqtt_ctx.is_running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int msg_id = mqtt_client_publish(topic, data, 0, qos, retain);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t mqtt_service_subscribe(const char *topic, int qos)
{
    if (!mqtt_ctx.is_connected || !mqtt_ctx.is_running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int msg_id = mqtt_client_subscribe(topic, qos);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t mqtt_service_unsubscribe(const char *topic)
{
    if (!mqtt_ctx.is_connected || !mqtt_ctx.is_running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int msg_id = mqtt_client_unsubscribe(topic);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

bool mqtt_service_can_publish(void) {
    return mqtt_ctx.is_running &&        // Service is running
           mqtt_ctx.is_connected &&      // MQTT is connected to broker
           ethernet_service_has_ip();    // Ethernet has valid IP
}
