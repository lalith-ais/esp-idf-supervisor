#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "mqtt_client.h"
#include "app_mqtt.h"

static const char *TAG = "mqtt_client";

// MQTT client handle
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Callback function pointers
static void (*message_callback)(const char *topic, const char *data, void *ctx) = NULL;
static void (*connection_callback)(bool connected, void *ctx) = NULL;
static void *message_callback_ctx = NULL;
static void *connection_callback_ctx = NULL;

esp_err_t mqtt_client_deinit(void)
{
    if (mqtt_client == NULL) {
        return ESP_OK;  // Already deinitialized
    }
    
    // Stop MQTT client
    esp_err_t ret = esp_mqtt_client_stop(mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop MQTT client: %s", esp_err_to_name(ret));
    }
    
    // Destroy MQTT client
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = NULL;
    mqtt_connected = false;
    
    ESP_LOGI(TAG, "MQTT client deinitialized");
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            mqtt_connected = true;
            
            if (connection_callback) {
                connection_callback(true, connection_callback_ctx);
            }
            
            // Subscribe to default topics if needed
            // mqtt_client_subscribe("esp32/p4/command", 0);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            mqtt_connected = false;
            
            if (connection_callback) {
                connection_callback(false, connection_callback_ctx);
            }
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            
            if (message_callback && event->topic_len > 0 && event->data_len > 0) {
                // Create null-terminated strings for topic and data
                char *topic = malloc(event->topic_len + 1);
                char *data = malloc(event->data_len + 1);
                
                if (topic && data) {
                    memcpy(topic, event->topic, event->topic_len);
                    topic[event->topic_len] = '\0';
                    
                    memcpy(data, event->data, event->data_len);
                    data[event->data_len] = '\0';
                    
                    message_callback(topic, data, message_callback_ctx);
                }
                
                free(topic);
                free(data);
            }
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", 
                         event->error_handle->esp_transport_sock_errno);
            }
            break;
            
        default:
            ESP_LOGD(TAG, "Other MQTT event id:%d", event_id);
            break;
    }
}

esp_err_t mqtt_client_init(const char *broker_uri, const char *client_id)
{
    if (broker_uri == NULL) {
        ESP_LOGE(TAG, "Broker URI cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
    };
    
    // Set client ID if provided
    if (client_id != NULL) {
        mqtt_cfg.credentials.client_id = client_id;
    }
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, 
                                   mqtt_event_handler, NULL);
    
    ESP_LOGI(TAG, "MQTT client initialized with broker: %s", broker_uri);
    return ESP_OK;
}

esp_err_t mqtt_client_start(void)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = esp_mqtt_client_start(mqtt_client);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client started");
    } else {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t mqtt_client_stop(void)
{
    if (mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = esp_mqtt_client_stop(mqtt_client);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client stopped");
        mqtt_connected = false;
    }
    
    return ret;
}

bool mqtt_client_is_connected(void)
{
    return mqtt_connected;
}

int mqtt_client_publish(const char *topic, const char *data, int len, int qos, int retain)
{
    if (mqtt_client == NULL || topic == NULL || data == NULL) {
        return -1;
    }
    
    if (len == 0) {
        len = strlen(data);
    }
    
    if (!mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, cannot publish");
        return -1;
    }
    
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, data, len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to topic: %s", topic);
    } else {
        ESP_LOGD(TAG, "Published to %s, msg_id=%d", topic, msg_id);
    }
    
    return msg_id;
}

int mqtt_client_subscribe(const char *topic, int qos)
{
    if (mqtt_client == NULL || topic == NULL) {
        return -1;
    }
    
    if (!mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, cannot subscribe");
        return -1;
    }
    
    int msg_id = esp_mqtt_client_subscribe(mqtt_client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to topic: %s", topic);
    } else {
        ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", topic, msg_id);
    }
    
    return msg_id;
}

int mqtt_client_unsubscribe(const char *topic)
{
    if (mqtt_client == NULL || topic == NULL) {
        return -1;
    }
    
    if (!mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, cannot unsubscribe");
        return -1;
    }
    
    int msg_id = esp_mqtt_client_unsubscribe(mqtt_client, topic);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to unsubscribe from topic: %s", topic);
    } else {
        ESP_LOGI(TAG, "Unsubscribed from %s, msg_id=%d", topic, msg_id);
    }
    
    return msg_id;
}

void mqtt_client_set_message_callback(void (*callback)(const char *topic, const char *data, void *ctx), void *user_context)
{
    message_callback = callback;
    message_callback_ctx = user_context;
}

void mqtt_client_set_connection_callback(void (*callback)(bool connected, void *ctx), void *user_context)
{
    connection_callback = callback;
    connection_callback_ctx = user_context;
}
