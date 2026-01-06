#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "esp_err.h"
#include "mqtt_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stop and clean up MQTT client
 * 
 * @return esp_err_t 
 */

esp_err_t mqtt_client_deinit(void); 

/**
 * @brief Initialize MQTT client
 * 
 * @param broker_uri MQTT broker URI (e.g., "mqtt://192.168.124.4")
 * @param client_id Client ID (can be NULL for auto-generated)
 * @return esp_err_t 
 */
esp_err_t mqtt_client_init(const char *broker_uri, const char *client_id);

/**
 * @brief Start MQTT client
 * 
 * @return esp_err_t 
 */
esp_err_t mqtt_client_start(void);

/**
 * @brief Stop MQTT client
 * 
 * @return esp_err_t 
 */
esp_err_t mqtt_client_stop(void);

/**
 * @brief Check if MQTT client is connected
 * 
 * @return true if connected
 * @return false if disconnected
 */
bool mqtt_client_is_connected(void);

/**
 * @brief Publish message to topic
 * 
 * @param topic Topic to publish to
 * @param data Message data
 * @param len Message length (0 for null-terminated strings)
 * @param qos QoS level (0, 1, or 2)
 * @param retain Retain flag
 * @return int Message ID or -1 on error
 */
int mqtt_client_publish(const char *topic, const char *data, int len, int qos, int retain);

/**
 * @brief Subscribe to topic
 * 
 * @param topic Topic to subscribe to
 * @param qos QoS level (0, 1, or 2)
 * @return int Message ID or -1 on error
 */
int mqtt_client_subscribe(const char *topic, int qos);

/**
 * @brief Unsubscribe from topic
 * 
 * @param topic Topic to unsubscribe from
 * @return int Message ID or -1 on error
 */
int mqtt_client_unsubscribe(const char *topic);

/**
 * @brief Set callback for received messages
 * 
 * @param callback Function to call when message is received
 * @param user_context User context passed to callback
 */
void mqtt_client_set_message_callback(void (*callback)(const char *topic, const char *data, void *ctx), void *user_context);

/**
 * @brief Set callback for connection status changes
 * 
 * @param callback Function to call when connection status changes
 * @param user_context User context passed to callback
 */
void mqtt_client_set_connection_callback(void (*callback)(bool connected, void *ctx), void *user_context);

#ifdef __cplusplus
}
#endif

#endif // APP_MQTT_H
