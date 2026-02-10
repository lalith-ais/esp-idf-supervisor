#include "dummy_temperature_service.h"
#include "mqtt_service.h"  // This gives us mqtt_service_can_publish()
#include "esp_log.h"
#include "freertos/task.h"
#include <stdlib.h>  // For rand()

static const char* TAG = "dummy-temp";

// Service context
typedef struct {
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    bool is_running;
    float last_temperature;
    uint32_t message_count;
} dummy_temp_context_t;

static dummy_temp_context_t dummy_ctx = {0};

// Generate random temperature between 20.0 and 30.0
static float generate_dummy_temperature(void) {
    // Simple pseudo-random temperature
    static float base_temp = 25.0f;
    float variation = ((rand() % 200) - 100) / 100.0f;  // -1.0 to +1.0
    return base_temp + variation;
}

// Internal task
static void dummy_temperature_task(void* arg) {
    ESP_LOGI(TAG, "Dummy temperature task starting");
    
    // Seed random number generator with current time
    srand(xTaskGetTickCount());
    
    while (dummy_ctx.is_running) {
        // Generate dummy temperature
        float temperature = generate_dummy_temperature();
        dummy_ctx.last_temperature = temperature;
        
        // Log
        ESP_LOGI(TAG, "Dummy temperature: %.2f°C (count: %lu)", 
                temperature, dummy_ctx.message_count);
        
        // Send to service queue (for supervisor monitoring)
        if (dummy_ctx.event_queue != NULL) {
            xQueueSend(dummy_ctx.event_queue, &temperature, 0);
        }
        
        // Publish via MQTT using your existing infrastructure
        if (mqtt_service_can_publish()) {  // Now this function is available
            char message[32];
            snprintf(message, sizeof(message), "%.2f", temperature);
            
            // Use YOUR EXISTING mqtt_service_publish function
            esp_err_t err = mqtt_service_publish("/ESP32P4/temperature", 
                                                message, 0, false);
            if (err == ESP_OK) {
                dummy_ctx.message_count++;
                ESP_LOGI(TAG, "Published to /ESP32P4/temperature: %s", message);
            } else {
                ESP_LOGW(TAG, "Failed to publish: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGD(TAG, "MQTT not ready, skipping publish");
        }
        
        // Wait 5 seconds as per your requirement
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    ESP_LOGI(TAG, "Dummy temperature task stopping");
    vTaskDelete(NULL);
}

// Public API implementation
void dummy_temperature_service_start(void) {
    if (dummy_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "Dummy temperature service already running");
        return;
    }
    
    // Create event queue
    dummy_ctx.event_queue = xQueueCreate(5, sizeof(float));
    
    // Start the dummy temperature task
    dummy_ctx.is_running = true;
    dummy_ctx.message_count = 0;
    dummy_ctx.last_temperature = 0.0f;
    
    xTaskCreate(dummy_temperature_task,
                "dummy-temp-task",
                4096,
                NULL,
                5,  // Lower priority than MQTT service (20)
                &dummy_ctx.task_handle);
    
    ESP_LOGI(TAG, "Dummy temperature service started");
}

QueueHandle_t dummy_temperature_service_get_queue(void) {
    return dummy_ctx.event_queue;
}

bool dummy_temperature_service_is_healthy(void) {
    return dummy_ctx.is_running && 
           (dummy_ctx.task_handle != NULL) &&
           (dummy_ctx.message_count < 1000000);  // Simple health check
}

uint32_t dummy_temperature_service_get_message_count(void) {
    return dummy_ctx.message_count;
}