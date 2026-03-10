/*
 * mqtt_service.c  (v1.3 -- LWT + health publish)
 *
 * CHANGES vs v1.2:
 *  [5] Last-Will-and-Testament (LWT)
 *      Broker auto-publishes  <status_topic>/status = "offline"  (retain=1,
 *      qos=1) if this node loses power or crashes hard without a clean
 *      disconnect.  On clean connect the service publishes "online" with
 *      retain=1 so subscribers always see the current state.
 *
 *  [6] Periodic health publish
 *      Every CONFIG_MQTT_HEALTH_INTERVAL_MS (default 30 s) the service
 *      publishes a JSON payload to <status_topic>/health:
 *        {"uptime_s":<N>,"heap_free":<N>,"ip":"x.x.x.x","crash":"<svc>"}
 *      "crash" is omitted when there is no recorded crash reason.
 *      This replaces the meaningless "Counter: N, Free Heap: N" payload
 *      from the old publish task.
 *
 * Status topic layout (all retained, qos=1):
 *   <status_topic>/status   -- "online" | "offline"  (LWT)
 *   <status_topic>/health   -- JSON health payload
 *
 * HARDENING retained from v1.2:
 *  [1] Queue-full logging
 *  [2] volatile bool
 *  [3] Heartbeat
 *  [4] Shutdown via queue
 */

#include "mqtt_service.h"
#include "app_mqtt.h"
#include "ethernet_service.h"
#include "supervisor.h"
#include "priorities.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt-service";

/* -------------------------------------------------------------------------
 * Default config from sdkconfig
 * ------------------------------------------------------------------------- */
#ifndef CONFIG_MQTT_BROKER_URI
#warning "CONFIG_MQTT_BROKER_URI not set in sdkconfig -- using compile-time fallback"
#define CONFIG_MQTT_BROKER_URI "mqtt://192.168.1.1"
#endif
#ifndef CONFIG_MQTT_CLIENT_ID
#define CONFIG_MQTT_CLIENT_ID        "ESP32P4-ETH"
#endif
#ifndef CONFIG_MQTT_PUBLISH_TOPIC
#define CONFIG_MQTT_PUBLISH_TOPIC    "/ESP32P4/NODE1"
#endif
#ifndef CONFIG_MQTT_SUBSCRIBE_TOPIC
#define CONFIG_MQTT_SUBSCRIBE_TOPIC  "/ESP32P4/COMMAND"
#endif
#ifndef CONFIG_MQTT_PUBLISH_INTERVAL_MS
#define CONFIG_MQTT_PUBLISH_INTERVAL_MS 5000
#endif
#ifndef CONFIG_MQTT_HEALTH_INTERVAL_MS
#define CONFIG_MQTT_HEALTH_INTERVAL_MS  30000   /* 30 s health publish */
#endif
#ifndef CONFIG_MQTT_NODE_ID
#define CONFIG_MQTT_NODE_ID             "NODE1"
#endif

/* Derived topic helpers -- built at runtime from status_topic */
/* status_topic  = publish_topic + "/status"  (e.g. /ESP32P4/NODE1/status) */
/* health_topic  = publish_topic + "/health"  (e.g. /ESP32P4/NODE1/health) */
#define MQTT_STATUS_SUFFIX   "/status"
#define MQTT_HEALTH_SUFFIX   "/health"

/* -------------------------------------------------------------------------
 * [1] Queue-send helper with drop warning
 * ------------------------------------------------------------------------- */
static inline void queue_send_warn(QueueHandle_t q,
                                   const mqtt_service_message_t *msg,
                                   const char *event_name)
{
    if (q == NULL) return;
    if (xQueueSend(q, msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full -- dropped event: %s", event_name);
    }
}

/* -------------------------------------------------------------------------
 * Service context
 * ------------------------------------------------------------------------- */
typedef struct {
    QueueHandle_t   event_queue;
    TaskHandle_t    task_handle;
    TaskHandle_t    publish_task_handle;
    volatile bool   is_running;          /* [2] */
    volatile bool   is_connected;        /* [2] */
    bool            publish_task_running;
    mqtt_config_t   config;
    uint32_t        message_counter;
} mqtt_service_ctx_t;

static mqtt_service_ctx_t s_ctx = {0};

static void mqtt_message_callback(const char *topic, const char *data, void *ctx);
static void mqtt_connection_callback(bool connected, void *ctx);

/* -------------------------------------------------------------------------
 * cleanup_and_exit -- centralised error teardown
 * ------------------------------------------------------------------------- */
static void cleanup_and_exit(esp_err_t err_code, const char *err_msg,
                              bool deinit_mqtt)
{
    s_ctx.is_running = false;

    mqtt_service_message_t msg = {
        .type = MQTT_SERVICE_EVENT_ERROR,
        .data.error.error_code = err_code,
    };
    strncpy(msg.data.error.error_msg, err_msg,
            sizeof(msg.data.error.error_msg) - 1);

    queue_send_warn(s_ctx.event_queue, &msg, "ERROR");  /* [1] */

    if (s_ctx.event_queue != NULL) {
        vQueueDelete(s_ctx.event_queue);
        s_ctx.event_queue = NULL;
    }

    if (deinit_mqtt) mqtt_client_deinit();

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
#endif
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * [6] Health publish task
 *
 * Publishes a JSON health payload to <publish_topic>/health every
 * health_interval_ms.  Topic and LWT are both derived from publish_topic
 * so a single sdkconfig key controls the whole topic tree.
 *
 * Example payload:
 *   {"uptime_s":3742,"heap_free":187432,"ip":"192.168.1.42","crash":"ethernet"}
 * "crash" key is only included when supervisor_get_last_crash() is non-NULL.
 * ------------------------------------------------------------------------- */
static void mqtt_publish_task(void *arg)
{
    ESP_LOGI(TAG, "Health publish task started");

    /* Boot time in us for uptime calculation */
    const int64_t boot_us = esp_timer_get_time();

    char health_topic[80];
    snprintf(health_topic, sizeof(health_topic), "%s%s",
             s_ctx.config.publish_topic, MQTT_HEALTH_SUFFIX);

    while (s_ctx.publish_task_running) {
        if (!s_ctx.is_connected || !s_ctx.is_running) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!ethernet_service_has_ip()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* Build JSON health payload */
        char payload[256];
        int64_t uptime_s = (esp_timer_get_time() - boot_us) / 1000000LL;
        uint32_t heap    = esp_get_free_heap_size();

        /* Get IP as string */
        char ip_str[16] = "0.0.0.0";
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }

        /* Optional crash key */
        const char *crash = supervisor_get_last_crash();
        if (crash) {
            snprintf(payload, sizeof(payload),
                     "{\"uptime_s\":%" PRId64
                     ",\"heap_free\":%" PRIu32
                     ",\"ip\":\"%s\""
                     ",\"crash\":\"%s\"}",
                     uptime_s, heap, ip_str, crash);
        } else {
            snprintf(payload, sizeof(payload),
                     "{\"uptime_s\":%" PRId64
                     ",\"heap_free\":%" PRIu32
                     ",\"ip\":\"%s\"}",
                     uptime_s, heap, ip_str);
        }

        int msg_id = mqtt_client_publish(health_topic, payload,
                                         strlen(payload), 1 /*qos*/, 0 /*retain*/);
        if (msg_id >= 0) {
            mqtt_service_message_t pub = {
                .type = MQTT_SERVICE_EVENT_PUBLISHED,
                .data.published.msg_id = msg_id,
            };
            strncpy(pub.data.published.topic, health_topic,
                    sizeof(pub.data.published.topic) - 1);
            queue_send_warn(s_ctx.event_queue, &pub, "HEALTH");  /* [1] */
            ESP_LOGI(TAG, "Health: %s", payload);
        } else {
            ESP_LOGW(TAG, "Health publish failed");
        }

        vTaskDelay(pdMS_TO_TICKS(s_ctx.config.health_interval_ms));
    }

    ESP_LOGI(TAG, "Health publish task stopping");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * MQTT service main task
 * ------------------------------------------------------------------------- */
static void mqtt_service_task(void *arg)
{
    ESP_LOGI(TAG, "MQTT service starting");

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    s_ctx.event_queue          = xQueueCreate(20, sizeof(mqtt_service_message_t));
    s_ctx.is_running           = true;
    s_ctx.task_handle          = xTaskGetCurrentTaskHandle();
    s_ctx.is_connected         = false;
    s_ctx.publish_task_running = false;
    s_ctx.message_counter      = 0;

    if (s_ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        cleanup_and_exit(ESP_ERR_NO_MEM, "queue alloc failed", false);
        return;
    }

    /* Apply defaults from sdkconfig if not overridden by caller */
    if (strlen(s_ctx.config.broker_uri) == 0) {
        strncpy(s_ctx.config.broker_uri,     CONFIG_MQTT_BROKER_URI,
                sizeof(s_ctx.config.broker_uri) - 1);
        strncpy(s_ctx.config.client_id,       CONFIG_MQTT_CLIENT_ID,
                sizeof(s_ctx.config.client_id) - 1);
        strncpy(s_ctx.config.publish_topic,   CONFIG_MQTT_PUBLISH_TOPIC,
                sizeof(s_ctx.config.publish_topic) - 1);
        strncpy(s_ctx.config.subscribe_topic, CONFIG_MQTT_SUBSCRIBE_TOPIC,
                sizeof(s_ctx.config.subscribe_topic) - 1);
        s_ctx.config.enabled             = true;
        s_ctx.config.publish_interval_ms = CONFIG_MQTT_PUBLISH_INTERVAL_MS;
        s_ctx.config.health_interval_ms  = CONFIG_MQTT_HEALTH_INTERVAL_MS;  /* [6] */
    }

    /* Build LWT topic: <publish_topic>/status */
    char lwt_topic[80];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s%s",
             s_ctx.config.publish_topic, MQTT_STATUS_SUFFIX);

    /* Wait for Ethernet IP */
    ESP_LOGI(TAG, "Waiting for Ethernet IP...");
    {
        const uint32_t TOTAL_MS = 120000;
        const uint32_t STEP_MS  = 500;
        uint32_t waited = 0;

        while (!ethernet_service_has_ip() && s_ctx.is_running) {
#ifdef CONFIG_ESP_TASK_WDT
            esp_task_wdt_reset();
#endif
            /* [3] Heartbeat even while waiting -- we are not stuck */
            supervisor_heartbeat("mqtt");

            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            waited += STEP_MS;
            if (waited >= TOTAL_MS) {
                ESP_LOGE(TAG, "Ethernet IP timeout");
                cleanup_and_exit(ESP_ERR_TIMEOUT, "Ethernet IP timeout", false);
                return;
            }
        }
    }

    /* Check for stop request that arrived while waiting */
    {
        mqtt_service_message_t peek;
        while (xQueuePeek(s_ctx.event_queue, &peek, 0) == pdTRUE) {
            xQueueReceive(s_ctx.event_queue, &peek, 0);
            if (peek.type == MQTT_SERVICE_EVENT_STOP_REQUESTED) {
                ESP_LOGI(TAG, "Stop requested before connect -- exiting");
                cleanup_and_exit(ESP_OK, "stop before connect", false);
                return;
            }
        }
    }

    ESP_LOGI(TAG, "Connecting to broker: %s (LWT: %s = offline)", s_ctx.config.broker_uri, lwt_topic);
    esp_err_t ret = mqtt_client_init(s_ctx.config.broker_uri,
                                      s_ctx.config.client_id,
                                      lwt_topic,      /* [5] LWT topic  */
                                      "offline",       /* [5] LWT message */
                                      1,               /* [5] LWT QoS    */
                                      1);              /* [5] LWT retain */
    if (ret != ESP_OK) {
        cleanup_and_exit(ret, "MQTT init failed", false);
        return;
    }

    mqtt_client_set_message_callback(mqtt_message_callback, &s_ctx);
    mqtt_client_set_connection_callback(mqtt_connection_callback, &s_ctx);

    ret = mqtt_client_start();
    if (ret != ESP_OK) {
        cleanup_and_exit(ret, "MQTT start failed", true);
        return;
    }

    {
        mqtt_service_message_t started = { .type = MQTT_SERVICE_EVENT_STARTED };
        queue_send_warn(s_ctx.event_queue, &started, "STARTED");  /* [1] */
    }

    /* Spawn publish task */
    s_ctx.publish_task_running = true;
    xTaskCreate(mqtt_publish_task, "mqtt-publish",
                4096, NULL, PRIO_MQTT_PUBLISH,
                &s_ctx.publish_task_handle);

    ESP_LOGI(TAG, "Running");

    /* Main loop */
    while (s_ctx.is_running) {

        /* [4] Check for stop request via queue */
        mqtt_service_message_t msg;
        if (xQueueReceive(s_ctx.event_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (msg.type == MQTT_SERVICE_EVENT_STOP_REQUESTED) {
                ESP_LOGI(TAG, "Stop requested -- shutting down");
                s_ctx.is_running = false;
                break;
            }
            /* Re-queue any other messages we consumed (supervisor wrapper reads them) */
            queue_send_warn(s_ctx.event_queue, &msg, "requeue");
        }

        if (!ethernet_service_has_ip()) {
            ESP_LOGW(TAG, "Lost Ethernet IP -- stopping MQTT client");
            mqtt_client_stop();
            s_ctx.is_connected = false;

            uint32_t wait_ms = 0;
            while (!ethernet_service_has_ip() && s_ctx.is_running) {
#ifdef CONFIG_ESP_TASK_WDT
                esp_task_wdt_reset();
#endif
                supervisor_heartbeat("mqtt");  /* [3] */
                vTaskDelay(pdMS_TO_TICKS(1000));
                wait_ms += 1000;
                if (wait_ms >= 30000) {
                    ESP_LOGE(TAG, "Ethernet reconnection timeout");
                    s_ctx.is_running = false;
                    break;
                }
            }
            if (s_ctx.is_running && ethernet_service_has_ip()) {
                ESP_LOGI(TAG, "Ethernet restored -- restarting MQTT");
                mqtt_client_start();
            }
        }

        /* [3] Pet heartbeat each iteration */
        supervisor_heartbeat("mqtt");

#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
#endif
    }

    /* Clean shutdown */
    ESP_LOGI(TAG, "Cleaning up...");

    s_ctx.publish_task_running = false;
    if (s_ctx.publish_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (eTaskGetState(s_ctx.publish_task_handle) != eDeleted) {
            vTaskDelete(s_ctx.publish_task_handle);
        }
        s_ctx.publish_task_handle = NULL;
    }

    mqtt_client_deinit();

    {
        mqtt_service_message_t stopped = { .type = MQTT_SERVICE_EVENT_STOPPED };
        queue_send_warn(s_ctx.event_queue, &stopped, "STOPPED");  /* [1] */
    }

    if (s_ctx.event_queue != NULL) {
        vQueueDelete(s_ctx.event_queue);
        s_ctx.event_queue = NULL;
    }

    s_ctx.task_handle = NULL;

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
#endif
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------- */
static void mqtt_message_callback(const char *topic, const char *data, void *ctx)
{
    ESP_LOGI(TAG, "RX: %s -> %s", topic, data);

    if (strcmp(topic, s_ctx.config.subscribe_topic) == 0) {
        if      (strcmp(data, "led_on")  == 0) ESP_LOGI(TAG, "CMD: LED ON");
        else if (strcmp(data, "led_off") == 0) ESP_LOGI(TAG, "CMD: LED OFF");
        else if (strcmp(data, "reboot")  == 0) ESP_LOGI(TAG, "CMD: reboot");
    }

    mqtt_service_message_t msg = { .type = MQTT_SERVICE_EVENT_MESSAGE_RECEIVED };
    strncpy(msg.data.message.topic, topic, sizeof(msg.data.message.topic) - 1);
    strncpy(msg.data.message.data,  data,  sizeof(msg.data.message.data)  - 1);
    queue_send_warn(s_ctx.event_queue, &msg, "MSG_RECEIVED");  /* [1] */
    s_ctx.message_counter++;
}

static void mqtt_connection_callback(bool connected, void *ctx)
{
    s_ctx.is_connected = connected;

    mqtt_service_message_t msg = {
        .type = connected ? MQTT_SERVICE_EVENT_CONNECTED
                          : MQTT_SERVICE_EVENT_DISCONNECTED,
    };

    if (connected) {
        ESP_LOGI(TAG, "MQTT connected");
        mqtt_client_subscribe(s_ctx.config.subscribe_topic, 0);

        /* [5] Publish "online" retained to status topic so Node-RED always
         * knows the node is alive.  The broker's LWT will publish "offline"
         * retained automatically on unclean disconnect. */
        char status_topic[80];
        snprintf(status_topic, sizeof(status_topic), "%s%s",
                 s_ctx.config.publish_topic, MQTT_STATUS_SUFFIX);
        mqtt_client_publish(status_topic, "online",
                            strlen("online"), 1 /*qos*/, 1 /*retain*/);
        ESP_LOGI(TAG, "Published: %s = online (retained)", status_topic);
    } else {
        ESP_LOGI(TAG, "MQTT disconnected");
    }

    queue_send_warn(s_ctx.event_queue, &msg,  /* [1] */
                    connected ? "CONNECTED" : "DISCONNECTED");
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void mqtt_service_start(void)
{
    if (s_ctx.task_handle != NULL) { ESP_LOGW(TAG, "Already running"); return; }
    xTaskCreate(mqtt_service_task, "mqtt-service",
                8192, NULL, PRIO_MQTT_SERVICE, &s_ctx.task_handle);
}

/* [4] Shutdown via queue -- task owns teardown */
void mqtt_service_stop(void)
{
    if (s_ctx.event_queue != NULL) {
        mqtt_service_message_t msg = { .type = MQTT_SERVICE_EVENT_STOP_REQUESTED };
        if (xQueueSend(s_ctx.event_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "Stop queue send failed");
        }
        vTaskDelay(pdMS_TO_TICKS(700));  /* allow graceful exit */
    }
    s_ctx.task_handle = NULL;
}

QueueHandle_t mqtt_service_get_queue(void)    { return s_ctx.event_queue;  }
bool mqtt_service_is_connected(void)           { return s_ctx.is_connected; }
bool mqtt_service_is_running(void)             { return s_ctx.is_running;   }
bool mqtt_service_can_publish(void) {
    return s_ctx.is_running && s_ctx.is_connected && ethernet_service_has_ip();
}

void mqtt_service_set_config(const mqtt_config_t *c) {
    if (c) memcpy(&s_ctx.config, c, sizeof(mqtt_config_t));
}
void mqtt_service_get_config(mqtt_config_t *c) {
    if (c) memcpy(c, &s_ctx.config, sizeof(mqtt_config_t));
}

esp_err_t mqtt_service_publish(const char *t, const char *d, int qos, bool retain) {
    if (!s_ctx.is_connected || !s_ctx.is_running) return ESP_ERR_INVALID_STATE;
    return (mqtt_client_publish(t, d, 0, qos, retain) >= 0) ? ESP_OK : ESP_FAIL;
}
esp_err_t mqtt_service_subscribe(const char *t, int qos) {
    if (!s_ctx.is_connected || !s_ctx.is_running) return ESP_ERR_INVALID_STATE;
    return (mqtt_client_subscribe(t, qos) >= 0) ? ESP_OK : ESP_FAIL;
}
esp_err_t mqtt_service_unsubscribe(const char *t) {
    if (!s_ctx.is_connected || !s_ctx.is_running) return ESP_ERR_INVALID_STATE;
    return (mqtt_client_unsubscribe(t) >= 0) ? ESP_OK : ESP_FAIL;
}
