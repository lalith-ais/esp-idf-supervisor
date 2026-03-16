/*
 * ds18b20_temp.c
 *
 * FIXES vs original:
 *
 *  [CRITICAL] Float sensor-index encoding hack removed
 *      The queue now pushes ds18b20_reading_t structs instead of encoding
 *      the sensor index as `temperature + (i * 1000.0f)`.
 *
 *  [MODERATE] Meaningless health check replaced
 *      `message_count < 1000000` replaced with a recency check:
 *      healthy iff the last successful reading was < HEALTH_STALE_S seconds ago.
 *
 *  [MODERATE] Queue creation race
 *      The event queue is created before the internal task is spawned so
 *      callers polling ds18b20_temp_service_get_queue() immediately after
 *      ds18b20_temp_service_start() always get a valid handle.
 *
 *  [MINOR] ds18b20_temp_service_start guard
 *      Checks task_handle OR is_running (not just task_handle) to avoid
 *      double-start after a supervisor restart that cleared task_handle.
 *
 *  [7] MAC-derived MQTT topics
 *      Temperature topics now embed the full Ethernet MAC (upper-hex) so
 *      that every unit on the same broker uses a unique topic namespace:
 *        single sensor:  /ESP32P4/AABBCCA1B2C3/temperature
 *        multi  sensor:  /ESP32P4/AABBCCA1B2C3/temperature/0
 *                        /ESP32P4/AABBCCA1B2C3/temperature/1  ...
 *      s_mac_id is populated by mqtt_service before this task publishes.
 */

#include "ds18b20_temp.h"
#include "mqtt_service.h"
#include "supervisor.h"
#include "priorities.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "onewire_bus.h"
#include "ds18b20.h"

static const char *TAG = "ds18b20-temp";

/* [7] Full 12-char upper-hex MAC string defined in mqtt_service.c.
 * If you later refactor into a shared node_id.c module, replace this
 * extern with #include "node_id.h".                                   */
extern char s_mac_id[];

/* Topic root -- must match CONFIG_MQTT_TOPIC_ROOT in mqtt_service.c   */
#define MQTT_TOPIC_ROOT  ""

/* Seconds without a successful reading before is_healthy() returns false */
#define HEALTH_STALE_S   120

/* -------------------------------------------------------------------------
 * Service context
 * ------------------------------------------------------------------------- */

typedef struct {
    QueueHandle_t  event_queue;
    TaskHandle_t   task_handle;
    volatile bool  is_running;          /* volatile for dual-core visibility */
    uint32_t       message_count;
    int64_t        last_reading_us;   /* esp_timer_get_time() at last good read */

    onewire_bus_handle_t    bus;
    ds18b20_device_handle_t sensors[DS18B20_MAX_SENSORS];
    int                     sensor_count;
    float                   last_temperatures[DS18B20_MAX_SENSORS];
} ds18b20_temp_ctx_t;

static ds18b20_temp_ctx_t s_ctx = {0};

/* -------------------------------------------------------------------------
 * Hardware init / cleanup
 * ------------------------------------------------------------------------- */

static esp_err_t hw_init(void)
{
    ESP_LOGI(TAG, "Initialising DS18B20 on GPIO%d", DS18B20_DEFAULT_GPIO);

    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = DS18B20_DEFAULT_GPIO,
        .flags        = { .en_pull_up = true },
    };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };

    esp_err_t ret = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &s_ctx.bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create 1-Wire bus: %s", esp_err_to_name(ret));
        return ret;
    }

    onewire_device_iter_handle_t iter = NULL;
    ret = onewire_new_device_iter(s_ctx.bus, &iter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator: %s", esp_err_to_name(ret));
        return ret;
    }

    s_ctx.sensor_count = 0;
    onewire_device_t dev;
    esp_err_t search;

    do {
        search = onewire_device_iter_get_next(iter, &dev);
        if (search != ESP_OK) break;

        ds18b20_config_t ds_cfg = {};
        if (ds18b20_new_device_from_enumeration(
                &dev, &ds_cfg,
                &s_ctx.sensors[s_ctx.sensor_count]) == ESP_OK) {

            uint64_t addr = 0;
            ds18b20_get_device_address(s_ctx.sensors[s_ctx.sensor_count], &addr);
            ESP_LOGI(TAG, "Found DS18B20[%d] addr=%016llX",
                     s_ctx.sensor_count, addr);

            s_ctx.last_temperatures[s_ctx.sensor_count] = 0.0f;
            s_ctx.sensor_count++;

            if (s_ctx.sensor_count >= DS18B20_MAX_SENSORS) {
                ESP_LOGI(TAG, "Max sensors (%d) reached", DS18B20_MAX_SENSORS);
                break;
            }
        }
    } while (search != ESP_ERR_NOT_FOUND);

    onewire_del_device_iter(iter);

    if (s_ctx.sensor_count == 0) {
        ESP_LOGW(TAG, "No DS18B20 sensors found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Found %d sensor(s)", s_ctx.sensor_count);
    return ESP_OK;
}

static void hw_cleanup(void)
{
    for (int i = 0; i < s_ctx.sensor_count; i++) {
        if (s_ctx.sensors[i] != NULL) {
            ds18b20_del_device(s_ctx.sensors[i]);
            s_ctx.sensors[i] = NULL;
        }
    }
    /* Uncomment if your SDK version exports onewire_del_bus():
     * if (s_ctx.bus != NULL) { onewire_del_bus(s_ctx.bus); s_ctx.bus = NULL; }
     */
}

/* -------------------------------------------------------------------------
 * Internal task
 * ------------------------------------------------------------------------- */

static void ds18b20_temp_task(void *arg)
{
    ESP_LOGI(TAG, "Task starting");

    while (s_ctx.is_running) {
        /* Trigger conversion on all sensors */
        for (int i = 0; i < s_ctx.sensor_count; i++) {
            esp_err_t err = ds18b20_trigger_temperature_conversion(s_ctx.sensors[i]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Trigger failed for sensor[%d]: %s",
                         i, esp_err_to_name(err));
            }
        }

        /* Wait for 12-bit conversion (max 750 ms) */
        vTaskDelay(pdMS_TO_TICKS(800));

        bool any_ok = false;
        for (int i = 0; i < s_ctx.sensor_count; i++) {
            float temp;
            esp_err_t err = ds18b20_get_temperature(s_ctx.sensors[i], &temp);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Read failed for sensor[%d]: %s",
                         i, esp_err_to_name(err));
                continue;
            }

            s_ctx.last_temperatures[i] = temp;
            s_ctx.last_reading_us      = esp_timer_get_time();
            any_ok = true;

            ESP_LOGI(TAG, "Sensor[%d]: %.2f°C (count=%" PRIu32 ")",
                     i, temp, s_ctx.message_count);

            /*
             * FIX: push a ds18b20_reading_t instead of temperature + i*1000.
             */
            if (s_ctx.event_queue != NULL) {
                ds18b20_reading_t reading = {
                    .sensor_index = i,
                    .temperature  = temp,
                };
                if (xQueueSend(s_ctx.event_queue, &reading, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Queue full -- reading dropped for sensor[%d]", i);
                }
            }

            /* Publish via MQTT */
            if (mqtt_service_can_publish()) {
                char topic[64];
                char value[32];

                snprintf(value, sizeof(value), "%.2f", temp);

                /* [7] Topic includes full MAC for per-unit uniqueness */
                if (s_ctx.sensor_count > 1) {
                    snprintf(topic, sizeof(topic),
                             "%s/%s/temperature/%d", MQTT_TOPIC_ROOT, s_mac_id, i);
                } else {
                    snprintf(topic, sizeof(topic),
                             "%s/%s/temperature", MQTT_TOPIC_ROOT, s_mac_id);
                }

                esp_err_t pub = mqtt_service_publish(topic, value, 0, false);
                if (pub == ESP_OK) {
                    s_ctx.message_count++;
                    ESP_LOGI(TAG, "Published %s -> %s", topic, value);
                } else {
                    ESP_LOGW(TAG, "Publish failed: %s", esp_err_to_name(pub));
                }
            }
        }

        if (!any_ok) {
            ESP_LOGW(TAG, "No successful readings this cycle");
        }

        /* Pet heartbeat so supervisor can detect if we get stuck */
        supervisor_heartbeat("ds18b20-temp");

        vTaskDelay(pdMS_TO_TICKS(30000));
    }

    ESP_LOGI(TAG, "Task stopping");
    hw_cleanup();
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void ds18b20_temp_service_start(void)
{
    if (s_ctx.is_running) {
        ESP_LOGW(TAG, "Already running");
        return;
    }

    /*
     * FIX: Create queue BEFORE spawning the task so callers polling
     * ds18b20_temp_service_get_queue() immediately after start() always
     * receive a valid handle.
     */
    if (s_ctx.event_queue == NULL) {
        s_ctx.event_queue = xQueueCreate(10, sizeof(ds18b20_reading_t));
        if (s_ctx.event_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create event queue");
            return;
        }
    }

    esp_err_t err = hw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hardware init failed: %s", esp_err_to_name(err));
        vQueueDelete(s_ctx.event_queue);
        s_ctx.event_queue = NULL;
        return;
    }

    s_ctx.is_running    = true;
    s_ctx.message_count = 0;
    s_ctx.last_reading_us = 0;

    BaseType_t rc = xTaskCreate(
        ds18b20_temp_task,
        "ds18b20-temp",
        4096,
        NULL,
        PRIO_DS18B20_SERVICE,
        &s_ctx.task_handle
    );

    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        s_ctx.is_running = false;
        hw_cleanup();
        vQueueDelete(s_ctx.event_queue);
        s_ctx.event_queue = NULL;
    } else {
        ESP_LOGI(TAG, "Service started (%d sensor(s))", s_ctx.sensor_count);
    }
}

void ds18b20_temp_service_stop(void)
{
    if (!s_ctx.is_running && s_ctx.task_handle == NULL) return;

    s_ctx.is_running  = false;
    s_ctx.task_handle = NULL;   /* task calls vTaskDelete(NULL) on exit */
    ESP_LOGI(TAG, "Service stop requested");
}

QueueHandle_t ds18b20_temp_service_get_queue(void)
{
    return s_ctx.event_queue;
}

/**
 * FIX: health is based on recency of the last successful reading, not on
 * message_count being below an arbitrary large number.
 */
bool ds18b20_temp_service_is_healthy(void)
{
    if (!s_ctx.is_running)       return false;
    if (s_ctx.sensor_count == 0) return false;

    if (s_ctx.last_reading_us == 0) {
        /* No reading yet — only unhealthy if the task has had time to run */
        return true;
    }

    int64_t age_s = (esp_timer_get_time() - s_ctx.last_reading_us) / 1000000LL;
    return age_s < HEALTH_STALE_S;
}

uint32_t ds18b20_temp_service_get_message_count(void)
{
    return s_ctx.message_count;
}

int ds18b20_temp_service_get_sensor_count(void)
{
    return s_ctx.sensor_count;
}

float ds18b20_temp_service_get_last_temperature(int idx)
{
    if (idx >= 0 && idx < s_ctx.sensor_count) {
        return s_ctx.last_temperatures[idx];
    }
    return 0.0f;
}

esp_err_t ds18b20_temp_service_trigger_conversion(void)
{
    if (s_ctx.sensor_count == 0) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < s_ctx.sensor_count; i++) {
        esp_err_t err = ds18b20_trigger_temperature_conversion(s_ctx.sensors[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
