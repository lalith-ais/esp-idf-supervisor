/*
 * system.c - Service supervisor tasks and registry
 *
 * CHANGES vs previous version:
 *
 *  [NET] ethernet_supervisor renamed network_supervisor.
 *        Now calls network_service_start/stop (transport-agnostic) instead
 *        of ethernet_service_start/stop.  The ethernet_transport vtable is
 *        passed in at startup -- swapping to wifi_transport is one line here.
 *
 *  [NET] ethernet_service.h replaced by network_service.h + ethernet_transport.h
 *
 *  [NET] Queue message type changed from eth_service_message_t / ETH_EVENT_*
 *        to net_service_message_t / NET_EVENT_*
 *
 *  [NET] mqtt_supervisor's stray ethernet_service_has_ip() call replaced with
 *        network_service_has_ip().
 *
 * Everything else (mqtt_supervisor, ds18b20_supervisor, display_supervisor,
 * restart policy, heartbeat, NVS crash, priority constants) is UNCHANGED.
 */

#include "system.h"
#include "priorities.h"
#include "network_service.h"        /* replaces ethernet_service.h */
#include "ethernet_transport.h"     /* swap to wifi_transport.h for WiFi */
#include "mqtt_service.h"
#include "ds18b20_temp.h"
#include "display_service.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* =========================================================================
 * Helpers
 * ========================================================================= */

static QueueHandle_t wait_for_queue(QueueHandle_t (*get_queue_fn)(void),
                                    uint32_t max_ms)
{
    const uint32_t step_ms = 10;
    uint32_t elapsed = 0;
    QueueHandle_t q = NULL;

    while (elapsed < max_ms) {
        q = get_queue_fn();
        if (q != NULL) return q;
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed += step_ms;
    }
    return NULL;
}

/* =========================================================================
 * network_supervisor  (was ethernet_supervisor)
 *
 * Passes the chosen transport vtable to network_service_start().
 * To switch transport: change &ethernet_transport to &wifi_transport here
 * and update the #include above.  Nothing else in the file changes.
 * ========================================================================= */

void network_supervisor(void *arg)
{
    static const char *TAG = "network-super";
    ESP_LOGI(TAG, "Starting (transport: %s)", ethernet_transport.name);

    network_service_stop();  /* clean up any prior inner task */
    network_service_start(&ethernet_transport);

    QueueHandle_t queue = wait_for_queue(network_service_get_queue, 500);
    if (queue == NULL) {
        ESP_LOGE(TAG, "Failed to obtain network event queue -- exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Running");

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    while (1) {
        net_service_message_t msg;

        if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (msg.type) {
                case NET_EVENT_CONNECTED:
                    ESP_LOGI(TAG, "Network connected");
                    break;
                case NET_EVENT_DISCONNECTED:
                    ESP_LOGW(TAG, "Network disconnected");
                    break;
                case NET_EVENT_GOT_IP:
                    ESP_LOGI(TAG, "Got IP: %s", msg.data.got_ip.ip);
                    break;
                case NET_EVENT_STARTED:
                    ESP_LOGI(TAG, "Network started");
                    break;
                case NET_EVENT_STOPPED:
                    ESP_LOGW(TAG, "Network stopped");
                    break;
                case NET_EVENT_ERROR:
                    ESP_LOGE(TAG, "Network hardware error -- exiting supervisor");
#ifdef CONFIG_ESP_TASK_WDT
                    esp_task_wdt_delete(NULL);
#endif
                    vTaskDelete(NULL);
                    return;
                default:
                    ESP_LOGW(TAG, "Unknown event: %d", msg.type);
                    break;
            }
        }

#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
#endif
    }

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
#endif
    vTaskDelete(NULL);
}

/* =========================================================================
 * mqtt_supervisor  -- UNCHANGED except one log line
 * ========================================================================= */

void mqtt_supervisor(void *arg)
{
    static const char *TAG = "mqtt-super";
    ESP_LOGI(TAG, "Starting");

    mqtt_service_stop();
    mqtt_service_start();

    QueueHandle_t queue = wait_for_queue(mqtt_service_get_queue, 500);
    if (queue == NULL) {
        ESP_LOGE(TAG, "Failed to obtain MQTT event queue -- exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Running");

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    while (1) {
        mqtt_service_message_t msg;

        if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (msg.type) {
                case MQTT_SERVICE_EVENT_CONNECTED:
                    ESP_LOGI(TAG, "MQTT connected");
                    break;
                case MQTT_SERVICE_EVENT_DISCONNECTED:
                    ESP_LOGW(TAG, "MQTT disconnected");
                    break;
                case MQTT_SERVICE_EVENT_MESSAGE_RECEIVED:
                    ESP_LOGI(TAG, "Message: %s -> %s",
                             msg.data.message.topic, msg.data.message.data);
                    break;
                case MQTT_SERVICE_EVENT_PUBLISHED:
                    ESP_LOGI(TAG, "Published to %s, msg_id=%d",
                             msg.data.published.topic, msg.data.published.msg_id);
                    break;
                case MQTT_SERVICE_EVENT_SUBSCRIBED:
                    ESP_LOGI(TAG, "Subscribed to %s, qos=%d",
                             msg.data.subscribed.topic, msg.data.subscribed.qos);
                    break;
                case MQTT_SERVICE_EVENT_STARTED:
                    ESP_LOGI(TAG, "MQTT service started");
                    break;
                case MQTT_SERVICE_EVENT_STOPPED:
                    ESP_LOGW(TAG, "MQTT service stopped");
                    break;
                case MQTT_SERVICE_EVENT_ERROR:
                    ESP_LOGE(TAG, "MQTT error: %s (0x%x)",
                             msg.data.error.error_msg, msg.data.error.error_code);
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown event: %d", msg.type);
                    break;
            }
        }

        if (!network_service_has_ip()) {
            ESP_LOGW(TAG, "No network IP -- MQTT service will handle reconnection");
        }

#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
#endif
    }

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
#endif
    vTaskDelete(NULL);
}

/* =========================================================================
 * ds18b20_temp_supervisor  -- UNCHANGED
 * ========================================================================= */

void ds18b20_temp_supervisor(void *arg)
{
    static const char *TAG = "ds18b20-super";
    ESP_LOGI(TAG, "DS18B20 temperature supervisor starting");

    ds18b20_temp_service_stop();
    ds18b20_temp_service_start();

    QueueHandle_t queue = wait_for_queue(ds18b20_temp_service_get_queue, 500);
    if (queue == NULL) {
        ESP_LOGE(TAG, "Failed to obtain DS18B20 event queue -- exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Running");

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    uint32_t last_message_count = 0;
    uint32_t stale_seconds      = 0;

    while (1) {
        ds18b20_reading_t reading;

        if (xQueueReceive(queue, &reading, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "Sensor[%d]: %.2f°C",
                     reading.sensor_index, reading.temperature);
            stale_seconds = 0;
        } else {
            uint32_t current_count = ds18b20_temp_service_get_message_count();
            if (current_count == last_message_count) {
                stale_seconds++;
                if (stale_seconds > 60) {
                    ESP_LOGW(TAG, "No new temperature data for %" PRIu32 " s",
                             stale_seconds);
                }
            } else {
                stale_seconds = 0;
            }
            last_message_count = current_count;
        }

        if (!ds18b20_temp_service_is_healthy()) {
            ESP_LOGW(TAG, "Health check failed");
        }

#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
#endif
    }

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
#endif
    vTaskDelete(NULL);
}

/* =========================================================================
 * display_supervisor  -- UNCHANGED
 * ========================================================================= */

void display_supervisor(void *arg)
{
    static const char *TAG = "display-super";
    ESP_LOGI(TAG, "Display supervisor starting");

    display_service_stop();
    display_service_start();

    while (1) {
        if (!display_service_is_running()) {
            ESP_LOGW(TAG, "Display service stopped unexpectedly");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    vTaskDelete(NULL);
}

/* =========================================================================
 * Service registry
 *
 * [NET] Entry renamed from "ethernet" to "network" and points to
 *       network_supervisor.
 *
 *       IMPORTANT: the service name here ("network") must match the
 *       supervisor_heartbeat() tag used inside network_service.c.
 *       network_service.c calls supervisor_heartbeat(transport->name),
 *       and ethernet_transport.name == "ethernet".
 *
 *       Therefore keep the registry name as "ethernet" while using
 *       ethernet_transport, and change it to "wifi" (or "network")
 *       when you switch -- it just has to match transport->name.
 * ========================================================================= */

const service_def_t services[] = {
    /* name        entry                stack   priority               restart        essential  ctx   hb_s */
    {"ethernet",   network_supervisor,  12288, PRIO_ETH_SUPERVISOR,  RESTART_ALWAYS, true,  NULL, 30},
    {"mqtt",       mqtt_supervisor,      8192, PRIO_MQTT_SUPERVISOR, RESTART_ALWAYS, false, NULL, 30},
    {"ds18b20-temp", ds18b20_temp_supervisor, 4096, PRIO_DS18B20_SUPERVISOR, RESTART_ALWAYS, false, NULL, 60},
    {"display",    display_supervisor,   4096, PRIO_DS18B20_SUPERVISOR, RESTART_ALWAYS, false, NULL, 0},
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL, 0}  /* sentinel */
};
