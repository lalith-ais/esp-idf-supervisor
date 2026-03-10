/*
 * ds18b20_temp.h
 *
 * FIXES vs original:
 *   - Added ds18b20_reading_t struct so the event queue carries typed data
 *     instead of a float with the sensor index encoded via ×1000 arithmetic.
 *   - ds18b20_temp_service_is_healthy() contract note added (see .c for fix).
 */

#ifndef DS18B20_TEMP_H
#define DS18B20_TEMP_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

#define DS18B20_MAX_SENSORS     4   /* Maximum DS18B20 sensors on the bus */
#define DS18B20_DEFAULT_GPIO    6   /* 1-Wire bus GPIO pin */

/* -------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

/**
 * @brief  Reading pushed onto the event queue by the service task.
 *
 * FIX: replaces the "temperature + index*1000" float hack.  Consumers now
 * receive an unambiguous struct with clearly typed fields.
 */
typedef struct {
    int   sensor_index;   /**< Zero-based index of the sensor */
    float temperature;    /**< Temperature in degrees Celsius */
} ds18b20_reading_t;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void           ds18b20_temp_service_start(void);
void           ds18b20_temp_service_stop(void);

/** Returns the event queue (items are ds18b20_reading_t, NOT raw float). */
QueueHandle_t  ds18b20_temp_service_get_queue(void);

/**
 * @brief  Basic health check.
 *
 * Returns true while the service is running, has found sensors, and has
 * produced a reading within the last HEALTH_STALE_THRESHOLD_S seconds.
 * (Previously the check was `message_count < 1000000` which is meaningless.)
 */
bool           ds18b20_temp_service_is_healthy(void);

uint32_t       ds18b20_temp_service_get_message_count(void);
int            ds18b20_temp_service_get_sensor_count(void);
float          ds18b20_temp_service_get_last_temperature(int sensor_index);
esp_err_t      ds18b20_temp_service_trigger_conversion(void);

#ifdef __cplusplus
}
#endif

#endif /* DS18B20_TEMP_H */
