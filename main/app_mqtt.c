/*
 * app_mqtt.c  (v1.3 -- LWT support)
 *
 * Thin wrapper around ESP-IDF's esp_mqtt_client.
 *
 * CHANGE vs v1.2:
 *  - mqtt_client_init() accepts lwt_topic/message/qos/retain and wires them
 *    into esp_mqtt_client_config_t.session.last_will.
 *  - publish() signature extended with explicit len parameter so binary
 *    payloads and pre-computed strlen() calls both work.
 */

#include "app_mqtt.h"
#include "mqtt_client.h"   /* ESP-IDF MQTT component */
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app-mqtt";

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
typedef struct {
    esp_mqtt_client_handle_t client;
    mqtt_message_cb_t        message_cb;
    void                    *message_ctx;
    mqtt_connection_cb_t     connection_cb;
    void                    *connection_ctx;
} app_mqtt_ctx_t;

static app_mqtt_ctx_t s_ctx = {0};

/* -------------------------------------------------------------------------
 * ESP-IDF MQTT event handler
 * ------------------------------------------------------------------------- */
static void mqtt_event_handler(void *handler_args,
                                esp_event_base_t base,
                                int32_t event_id,
                                void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        if (s_ctx.connection_cb) {
            s_ctx.connection_cb(true, s_ctx.connection_ctx);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        if (s_ctx.connection_cb) {
            s_ctx.connection_cb(false, s_ctx.connection_ctx);
        }
        break;

    case MQTT_EVENT_DATA:
        if (event->topic && event->data) {
            /* Null-terminate safely into local buffers */
            char topic[128] = {0};
            char data[512]  = {0};
            int  tlen = (event->topic_len < (int)sizeof(topic) - 1)
                        ? event->topic_len : (int)sizeof(topic) - 1;
            int  dlen = (event->data_len  < (int)sizeof(data)  - 1)
                        ? event->data_len  : (int)sizeof(data)  - 1;
            memcpy(topic, event->topic, tlen);
            memcpy(data,  event->data,  dlen);

            if (s_ctx.message_cb) {
                s_ctx.message_cb(topic, data, s_ctx.message_ctx);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT_EVENT_ERROR");
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
esp_err_t mqtt_client_init(const char *broker_uri,
                            const char *client_id,
                            const char *lwt_topic,
                            const char *lwt_message,
                            int         lwt_qos,
                            int         lwt_retain)
{
    if (s_ctx.client != NULL) {
        ESP_LOGW(TAG, "Already initialised -- call deinit first");
        return ESP_ERR_INVALID_STATE;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.client_id = client_id,
    };

    /* [v1.3] Wire LWT if a topic was provided */
    if (lwt_topic && lwt_topic[0] != '\0') {
        mqtt_cfg.session.last_will.topic   = lwt_topic;
        mqtt_cfg.session.last_will.msg     = lwt_message ? lwt_message : "";
        mqtt_cfg.session.last_will.msg_len = lwt_message ? strlen(lwt_message) : 0;
        mqtt_cfg.session.last_will.qos     = lwt_qos;
        mqtt_cfg.session.last_will.retain  = lwt_retain;
        ESP_LOGI(TAG, "LWT configured: topic=%s msg=%s qos=%d retain=%d",
                 lwt_topic, lwt_message, lwt_qos, lwt_retain);
    }

    s_ctx.client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_ctx.client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_mqtt_client_register_event(
        s_ctx.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT events: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_ctx.client);
        s_ctx.client = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client initialised: broker=%s client_id=%s",
             broker_uri, client_id);
    return ESP_OK;
}

esp_err_t mqtt_client_start(void)
{
    if (s_ctx.client == NULL) return ESP_ERR_INVALID_STATE;
    return esp_mqtt_client_start(s_ctx.client);
}

esp_err_t mqtt_client_stop(void)
{
    if (s_ctx.client == NULL) return ESP_ERR_INVALID_STATE;
    return esp_mqtt_client_stop(s_ctx.client);
}

void mqtt_client_deinit(void)
{
    if (s_ctx.client != NULL) {
        esp_mqtt_client_destroy(s_ctx.client);
        s_ctx.client = NULL;
    }
}

int mqtt_client_publish(const char *topic, const char *data,
                        size_t len, int qos, int retain)
{
    if (s_ctx.client == NULL) return -1;
    int payload_len = (len > 0) ? (int)len : (int)strlen(data);
    return esp_mqtt_client_publish(s_ctx.client, topic, data,
                                    payload_len, qos, retain);
}

int mqtt_client_subscribe(const char *topic, int qos)
{
    if (s_ctx.client == NULL) return -1;
    return esp_mqtt_client_subscribe(s_ctx.client, topic, qos);
}

int mqtt_client_unsubscribe(const char *topic)
{
    if (s_ctx.client == NULL) return -1;
    return esp_mqtt_client_unsubscribe(s_ctx.client, topic);
}

void mqtt_client_set_message_callback(mqtt_message_cb_t cb, void *ctx)
{
    s_ctx.message_cb  = cb;
    s_ctx.message_ctx = ctx;
}

void mqtt_client_set_connection_callback(mqtt_connection_cb_t cb, void *ctx)
{
    s_ctx.connection_cb  = cb;
    s_ctx.connection_ctx = ctx;
}
