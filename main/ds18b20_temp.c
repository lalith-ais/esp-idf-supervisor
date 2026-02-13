#include "ds18b20_temp.h"
#include "mqtt_service.h"  // This gives us mqtt_service_can_publish()
#include "esp_log.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "onewire_bus.h"
#include "ds18b20.h"

static const char* TAG = "ds18b20-temp";

// Service context
typedef struct {
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    bool is_running;
    uint32_t message_count;
    
    // DS18B20 specific context
    onewire_bus_handle_t bus;
    ds18b20_device_handle_t sensors[DS18B20_MAX_SENSORS];
    int sensor_count;
    float last_temperatures[DS18B20_MAX_SENSORS];
} ds18b20_temp_context_t;

static ds18b20_temp_context_t ds18b20_ctx = {0};

// Initialize the 1-Wire bus and enumerate DS18B20 devices
static esp_err_t ds18b20_init_hardware(void) {
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Initializing DS18B20 on GPIO%d", DS18B20_DEFAULT_GPIO);
    
    // Install 1-Wire bus
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = DS18B20_DEFAULT_GPIO,
        .flags = {
            .en_pull_up = true,  // Enable internal pull-up resistor
        }
    };
    
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,  // 1byte ROM command + 8byte ROM number + 1byte device command
    };
    
    ret = onewire_new_bus_rmt(&bus_config, &rmt_config, &ds18b20_ctx.bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create 1-Wire bus: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create device iterator for searching
    onewire_device_iter_handle_t iter = NULL;
    ret = onewire_new_device_iter(ds18b20_ctx.bus, &iter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create device iterator: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Device iterator created, start searching for DS18B20 sensors...");
    
    // Search for devices
    ds18b20_ctx.sensor_count = 0;
    onewire_device_t next_device;
    esp_err_t search_result;
    
    do {
        search_result = onewire_device_iter_get_next(iter, &next_device);
        if (search_result == ESP_OK) {
            // Found a new device, check if it's a DS18B20
            ds18b20_config_t ds_cfg = {};
            
            if (ds18b20_new_device_from_enumeration(&next_device, &ds_cfg, 
                    &ds18b20_ctx.sensors[ds18b20_ctx.sensor_count]) == ESP_OK) {
                
                uint64_t address;
                ds18b20_get_device_address(ds18b20_ctx.sensors[ds18b20_ctx.sensor_count], &address);
                
                ESP_LOGI(TAG, "Found DS18B20[%d], address: %016llX", 
                        ds18b20_ctx.sensor_count, address);
                
                ds18b20_ctx.sensor_count++;
                ds18b20_ctx.last_temperatures[ds18b20_ctx.sensor_count - 1] = 0.0f;
                
                if (ds18b20_ctx.sensor_count >= DS18B20_MAX_SENSORS) {
                    ESP_LOGI(TAG, "Reached maximum sensor count (%d)", DS18B20_MAX_SENSORS);
                    break;
                }
            } else {
                ESP_LOGI(TAG, "Found unknown device, address: %016llX", next_device.address);
            }
        }
    } while (search_result != ESP_ERR_NOT_FOUND);
    
    // Clean up iterator
    if (iter != NULL) {
        onewire_del_device_iter(iter);
    }
    
    if (ds18b20_ctx.sensor_count == 0) {
        ESP_LOGW(TAG, "No DS18B20 sensors found!");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Search complete, found %d DS18B20 sensor(s)", ds18b20_ctx.sensor_count);
    return ESP_OK;
}

// Clean up hardware resources
static void ds18b20_cleanup_hardware(void) {
    ESP_LOGI(TAG, "Cleaning up DS18B20 hardware");
    
    // Delete all sensor devices
    for (int i = 0; i < ds18b20_ctx.sensor_count; i++) {
        if (ds18b20_ctx.sensors[i] != NULL) {
            ds18b20_del_device(ds18b20_ctx.sensors[i]);
            ds18b20_ctx.sensors[i] = NULL;
        }
    }
    
    // Note: There might not be a bus delete function in your version
    // If onewire_del_bus exists, uncomment this:
    // if (ds18b20_ctx.bus != NULL) {
    //     onewire_del_bus(ds18b20_ctx.bus);
    //     ds18b20_ctx.bus = NULL;
    // }
}

// Internal task
static void ds18b20_temp_task(void* arg) {
    ESP_LOGI(TAG, "DS18B20 temperature task starting");
    
    while (ds18b20_ctx.is_running) {
        // Trigger temperature conversion for each sensor individually
        // or use broadcast if all sensors are on the same bus
        bool any_success = false;
        
        for (int i = 0; i < ds18b20_ctx.sensor_count; i++) {
            // Trigger conversion for this specific sensor
            esp_err_t err = ds18b20_trigger_temperature_conversion(ds18b20_ctx.sensors[i]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to trigger conversion for sensor[%d]: %s", 
                        i, esp_err_to_name(err));
                continue;
            }
        }
        
        // Wait for conversion to complete (max 750ms for 12-bit resolution)
        vTaskDelay(pdMS_TO_TICKS(800));
        
        // Read temperatures from all sensors
        float temperature;
        
        for (int i = 0; i < ds18b20_ctx.sensor_count; i++) {
            esp_err_t err = ds18b20_get_temperature(ds18b20_ctx.sensors[i], &temperature);
            
            if (err == ESP_OK) {
                ds18b20_ctx.last_temperatures[i] = temperature;
                
                ESP_LOGI(TAG, "DS18B20[%d] temperature: %.2f°C (count: %lu)", 
                        i, temperature, ds18b20_ctx.message_count);
                
                // Send to service queue (for supervisor monitoring)
                if (ds18b20_ctx.event_queue != NULL) {
                    // Send temperature with sensor index encoded in the float
                    float temp_with_index = temperature + (i * 1000.0f); // Hack to encode index
                    xQueueSend(ds18b20_ctx.event_queue, &temp_with_index, 0);
                }
                
                any_success = true;
                
                // Publish via MQTT using your existing infrastructure
                if (mqtt_service_can_publish()) {
                    char message[32];
                    snprintf(message, sizeof(message), "%.2f", temperature);
                    
                    // Create topic with sensor index
                    char topic[64];
                    if (ds18b20_ctx.sensor_count > 1) {
                        snprintf(topic, sizeof(topic), "/ESP32P4/temperature/%d", i);
                    } else {
                        snprintf(topic, sizeof(topic), "/ESP32P4/temperature");
                    }
                    
                    err = mqtt_service_publish(topic, message, 0, false);
                    if (err == ESP_OK) {
                        ds18b20_ctx.message_count++;
                        ESP_LOGI(TAG, "Published to %s: %s", topic, message);
                    } else {
                        ESP_LOGW(TAG, "Failed to publish to %s: %s", topic, esp_err_to_name(err));
                    }
                }
            } else {
                ESP_LOGW(TAG, "Failed to read temperature from DS18B20[%d]: %s", 
                        i, esp_err_to_name(err));
            }
        }
        
        if (!any_success) {
            ESP_LOGW(TAG, "Failed to read any temperature sensors");
        }
        
        // Wait 5 seconds as per your requirement
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    ESP_LOGI(TAG, "DS18B20 temperature task stopping");
    
    // Clean up hardware
    ds18b20_cleanup_hardware();
    
    vTaskDelete(NULL);
}

// Public API implementation
void ds18b20_temp_service_start(void) {
    if (ds18b20_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "DS18B20 temperature service already running");
        return;
    }
    
    // Create event queue
    ds18b20_ctx.event_queue = xQueueCreate(10, sizeof(float));  // Queue for temperatures
    
    // Initialize hardware
    esp_err_t err = ds18b20_init_hardware();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize DS18B20 hardware: %s", esp_err_to_name(err));
        // Clean up queue if hardware init failed
        if (ds18b20_ctx.event_queue != NULL) {
            vQueueDelete(ds18b20_ctx.event_queue);
            ds18b20_ctx.event_queue = NULL;
        }
        return;
    }
    
    // Start the DS18B20 temperature task
    ds18b20_ctx.is_running = true;
    ds18b20_ctx.message_count = 0;
    
    xTaskCreate(ds18b20_temp_task,
                "ds18b20-temp-task",
                4096,  // Stack size
                NULL,
                5,     // Priority (lower than MQTT service's 20)
                &ds18b20_ctx.task_handle);
    
    ESP_LOGI(TAG, "DS18B20 temperature service started with %d sensor(s)", ds18b20_ctx.sensor_count);
}

void ds18b20_temp_service_stop(void) {
    if (ds18b20_ctx.task_handle != NULL) {
        ds18b20_ctx.is_running = false;
        // Task will clean up and delete itself
        ds18b20_ctx.task_handle = NULL;
        ESP_LOGI(TAG, "DS18B20 temperature service stopping");
    }
}

QueueHandle_t ds18b20_temp_service_get_queue(void) {
    return ds18b20_ctx.event_queue;
}

bool ds18b20_temp_service_is_healthy(void) {
    return ds18b20_ctx.is_running && 
           (ds18b20_ctx.task_handle != NULL) &&
           (ds18b20_ctx.sensor_count > 0) &&
           (ds18b20_ctx.message_count < 1000000);  // Simple health check
}

uint32_t ds18b20_temp_service_get_message_count(void) {
    return ds18b20_ctx.message_count;
}

int ds18b20_temp_service_get_sensor_count(void) {
    return ds18b20_ctx.sensor_count;
}

float ds18b20_temp_service_get_last_temperature(int sensor_index) {
    if (sensor_index >= 0 && sensor_index < ds18b20_ctx.sensor_count) {
        return ds18b20_ctx.last_temperatures[sensor_index];
    }
    return 0.0f;
}

esp_err_t ds18b20_temp_service_trigger_conversion(void) {
    if (ds18b20_ctx.sensor_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Trigger conversion for all sensors
    for (int i = 0; i < ds18b20_ctx.sensor_count; i++) {
        esp_err_t err = ds18b20_trigger_temperature_conversion(ds18b20_ctx.sensors[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    
    return ESP_OK;
}