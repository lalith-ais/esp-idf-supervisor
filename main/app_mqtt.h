/*
 * app_mqtt.h  (v1.3 -- LWT support)
 *
 * Thin wrapper around ESP-IDF's esp_mqtt_client.
 *
 * CHANGE vs v1.2:
 *  - mqtt_client_init() gains four LWT parameters:
 *      lwt_topic, lwt_message, lwt_qos, lwt_retain
 *    These map directly to the ESP-IDF mqtt_cfg.session.last_will fields.
 *    Pass NULL for lwt_topic to disable LWT.
 */

#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the MQTT client.
 *
 * @param broker_uri   e.g. "mqtt://192.168.1.100"
 * @param client_id    Unique client ID string
 * @param lwt_topic    Last-will topic (NULL to disable LWT)
 * @param lwt_message  Last-will payload string
 * @param lwt_qos      Last-will QoS (0, 1, or 2)
 * @param lwt_retain   1 = broker retains LWT message
 */
esp_err_t mqtt_client_init(const char *broker_uri,
                            const char *client_id,
                            const char *lwt_topic,
                            const char *lwt_message,
                            int         lwt_qos,
                            int         lwt_retain);

esp_err_t mqtt_client_start(void);
esp_err_t mqtt_client_stop(void);
void      mqtt_client_deinit(void);

/**
 * Publish a message.
 * @param len  Payload length in bytes; pass 0 to use strlen(data).
 * @return message ID >= 0 on success, -1 on failure.
 */
int mqtt_client_publish(const char *topic, const char *data,
                        size_t len, int qos, int retain);

int mqtt_client_subscribe(const char *topic, int qos);
int mqtt_client_unsubscribe(const char *topic);

/** Callbacks registered by mqtt_service */
typedef void (*mqtt_message_cb_t)(const char *topic, const char *data, void *ctx);
typedef void (*mqtt_connection_cb_t)(bool connected, void *ctx);

void mqtt_client_set_message_callback(mqtt_message_cb_t cb, void *ctx);
void mqtt_client_set_connection_callback(mqtt_connection_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* APP_MQTT_H */
