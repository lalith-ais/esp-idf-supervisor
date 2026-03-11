/*
 * system.c - Service supervisor tasks and registry
 *
 * FIXES vs original system.h (inline definitions):
 *
 *  [CRITICAL] system.h contained function & variable definitions
 *      All definitions are now here; system.h only has declarations.
 *
 *  [CRITICAL] Orphaned inner tasks on supervisor restart
 *      Each supervisor now calls the service's _stop() before _start() so
 *      that an inner task left running from a previous incarnation is cleanly
 *      torn down before a new one is created.
 *
 *  [CRITICAL] DS18B20 float sensor-index encoding hack
 *      Removed entirely.  The queue now carries a proper struct
 *      (ds18b20_reading_t defined in ds18b20_temp.h) with explicit fields.
 *      See ds18b20_temp.c / ds18b20_temp.h for the matching change.
 *
 *  [MINOR] Typos in comments fixed ("temerature servoce" → "temperature service")
 *  [MINOR] Inconsistent log tags unified per supervisor.
 *  [MINOR] Unused DEFINE_SERVICE_SUPERVISOR macro removed.
 *  [MINOR] Priority magic numbers replaced with constants from priorities.h.
 */

#include "system.h"
#include "priorities.h"
#include "ethernet_service.h"
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

/**
 * @brief  Wait up to @p max_ms for @p get_queue_fn to return a non-NULL handle.
 * @return The queue handle, or NULL on timeout.
 */
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
 * ethernet_supervisor
 * ========================================================================= */

void ethernet_supervisor(void *arg)
{
    static const char *TAG = "ethernet-super";
    ESP_LOGI(TAG, "Starting");

    /*
     * FIX: Stop any previous inner service before starting a new one.
     * This prevents orphaned tasks when the supervisor itself is restarted.
     */
    ethernet_service_stop();

    ethernet_service_start();

    QueueHandle_t queue = wait_for_queue(ethernet_service_get_queue, 500);
    if (queue == NULL) {
        ESP_LOGE(TAG, "Failed to obtain Ethernet event queue — exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Running");

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    while (1) {
        eth_service_message_t msg;

        if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (msg.type) {
                case ETH_EVENT_CONNECTED:
                    ESP_LOGI(TAG, "Ethernet connected");
                    break;
                case ETH_EVENT_DISCONNECTED:
                    ESP_LOGW(TAG, "Ethernet disconnected");
                    break;
                case ETH_EVENT_GOT_IP:
                    ESP_LOGI(TAG, "Got IP: %s", msg.data.got_ip.ip);
                    break;
                case ETH_EVENT_STARTED:
                    ESP_LOGI(TAG, "Ethernet started");
                    break;
                case ETH_EVENT_STOPPED:
                    ESP_LOGW(TAG, "Ethernet stopped");
                    break;
                case ETH_EVENT_ERROR:
                    ESP_LOGE(TAG, "Ethernet hardware error — exiting supervisor");
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
 * mqtt_supervisor
 * ========================================================================= */

void mqtt_supervisor(void *arg)
{
    static const char *TAG = "mqtt-super";
    ESP_LOGI(TAG, "Starting");

    /* FIX: tear down any leftover inner task first */
    mqtt_service_stop();

    mqtt_service_start();

    QueueHandle_t queue = wait_for_queue(mqtt_service_get_queue, 500);
    if (queue == NULL) {
        ESP_LOGE(TAG, "Failed to obtain MQTT event queue — exiting");
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
                    ESP_LOGI(TAG, "Message: %s → %s",
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

        if (!ethernet_service_has_ip()) {
            ESP_LOGW(TAG, "No Ethernet IP — MQTT service will handle reconnection");
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
 * ds18b20_temp_supervisor
 * ========================================================================= */

void ds18b20_temp_supervisor(void *arg)
{
    static const char *TAG = "ds18b20-super";
    ESP_LOGI(TAG, "DS18B20 temperature supervisor starting");

    /* FIX: tear down any leftover inner task first */
    ds18b20_temp_service_stop();

    ds18b20_temp_service_start();

    QueueHandle_t queue = wait_for_queue(ds18b20_temp_service_get_queue, 500);
    if (queue == NULL) {
        ESP_LOGE(TAG, "Failed to obtain DS18B20 event queue — exiting");
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
        /*
         * FIX: queue now carries ds18b20_reading_t (sensor_index + temperature)
         * instead of a float with the index packed in via ×1000 arithmetic.
         */
        ds18b20_reading_t reading;

        if (xQueueReceive(queue, &reading, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ESP_LOGI(TAG, "Sensor[%d]: %.2f°C", reading.sensor_index, reading.temperature);
            stale_seconds = 0;
        } else {
            /* No item in queue this second — check whether the service is progressing */
            uint32_t current_count = ds18b20_temp_service_get_message_count();
            if (current_count == last_message_count) {
                stale_seconds++;
                if (stale_seconds > 10) {
                    ESP_LOGW(TAG, "No new temperature data for %"PRIu32" s", stale_seconds);
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
 * display_supervisor
 *
 * Manages the TM1637 display service.  The display service has no event
 * queue of its own that the supervisor needs to drain -- it is self-contained.
 * The supervisor simply starts it, then loops doing periodic health checks.
 * ========================================================================= */

void display_supervisor(void *arg)
{
    static const char *TAG = "display-super";
    ESP_LOGI(TAG, "Display supervisor starting");

    display_service_stop();   /* clean up any prior instance */
    display_service_start();

    while (1) {
        if (!display_service_is_running()) {
            ESP_LOGW(TAG, "Display service stopped unexpectedly");
            /* Supervisor will handle restart via restart policy */
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    vTaskDelete(NULL);
}

/* =========================================================================
 * Service registry
 *
 * FIX: priority values now come from priorities.h — no magic numbers.
 * ========================================================================= */

const service_def_t services[] = {
    /* name           entry                    stack   priority                  restart        essential  ctx   heartbeat_s */
    {"ethernet",     ethernet_supervisor,      12288, PRIO_ETH_SUPERVISOR,    RESTART_ALWAYS, true,  NULL, 30},
    {"mqtt",         mqtt_supervisor,           8192, PRIO_MQTT_SUPERVISOR,   RESTART_ALWAYS, false, NULL, 30},
    {"ds18b20-temp", ds18b20_temp_supervisor,   4096, PRIO_DS18B20_SUPERVISOR,RESTART_ALWAYS, false, NULL, 60},
    {"display",      display_supervisor,        4096, PRIO_DS18B20_SUPERVISOR,RESTART_ALWAYS, false, NULL, 0},
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL, 0}   /* sentinel */
};
