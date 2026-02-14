#ifndef DS18B20_TEMP_H
#define DS18B20_TEMP_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configuration constants
#define DS18B20_MAX_SENSORS     4   // Maximum number of DS18B20 sensors supported
#define DS18B20_DEFAULT_GPIO    6   // 

// Public API matching your pattern
void ds18b20_temp_service_start(void);
void ds18b20_temp_service_stop(void);
QueueHandle_t ds18b20_temp_service_get_queue(void);
bool ds18b20_temp_service_is_healthy(void);

// Add getter for message count (so supervisor can access it)
uint32_t ds18b20_temp_service_get_message_count(void);

// Additional useful functions
int ds18b20_temp_service_get_sensor_count(void);
float ds18b20_temp_service_get_last_temperature(int sensor_index);
esp_err_t ds18b20_temp_service_trigger_conversion(void);

#ifdef __cplusplus
}
#endif

#endif // DS18B20_TEMP_H
