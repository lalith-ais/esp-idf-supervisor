#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// MQTT service messages - Renamed to avoid conflicts with ESP-IDF MQTT enum
typedef enum {
    MQTT_SERVICE_EVENT_CONNECTED,
    MQTT_SERVICE_EVENT_DISCONNECTED,
    MQTT_SERVICE_EVENT_MESSAGE_RECEIVED,
    MQTT_SERVICE_EVENT_PUBLISHED,
    MQTT_SERVICE_EVENT_SUBSCRIBED,
    MQTT_SERVICE_EVENT_ERROR,
    MQTT_SERVICE_EVENT_STARTED,
    MQTT_SERVICE_EVENT_STOPPED
} mqtt_service_event_type_t;

typedef struct {
    mqtt_service_event_type_t type;
    union {
        struct {
            char topic[64];
            char data[256];
        } message;
        struct {
            char topic[64];
            int msg_id;
        } published;
        struct {
            char topic[64];
            int qos;
            int msg_id;
        } subscribed;
        struct {
            esp_err_t error_code;
            char error_msg[64];
        } error;
    } data;
} mqtt_service_message_t;

// Configuration structure
typedef struct {
    char broker_uri[128];
    char client_id[64];
    char publish_topic[64];
    char subscribe_topic[64];
    bool enabled;
    int publish_interval_ms;
} mqtt_config_t;

// Public API
void mqtt_service_start(void);
void mqtt_service_stop(void);
QueueHandle_t mqtt_service_get_queue(void);
bool mqtt_service_is_connected(void);
bool mqtt_service_is_running(void);

// Configuration API
void mqtt_service_set_config(const mqtt_config_t *config);
void mqtt_service_get_config(mqtt_config_t *config);

// Control API
esp_err_t mqtt_service_publish(const char *topic, const char *data, int qos, bool retain);
esp_err_t mqtt_service_subscribe(const char *topic, int qos);
esp_err_t mqtt_service_unsubscribe(const char *topic);

#ifdef __cplusplus
}
#endif

#endif // MQTT_SERVICE_H