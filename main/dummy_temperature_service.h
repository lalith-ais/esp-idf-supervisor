#ifndef DUMMY_TEMPERATURE_SERVICE_H
#define DUMMY_TEMPERATURE_SERVICE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Public API matching your pattern
void dummy_temperature_service_start(void);
QueueHandle_t dummy_temperature_service_get_queue(void);
bool dummy_temperature_service_is_healthy(void);

// Add getter for message count (so supervisor can access it)
uint32_t dummy_temperature_service_get_message_count(void);

#ifdef __cplusplus
}
#endif

#endif // DUMMY_TEMPERATURE_SERVICE_H