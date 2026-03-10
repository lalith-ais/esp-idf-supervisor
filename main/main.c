// main.c - Bootloader with proper ESP-IDF initialization
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "system.h"

void app_main(void)
{
    // Set log level
    esp_log_level_set("*", ESP_LOG_INFO);
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI("main", "NVS needs erase, doing it...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI("main", "Bootloader starting. Heap free: %"PRIu32, 
            esp_get_free_heap_size());
    
    // --- CRITICAL: Initialize ESP-IDF networking ONCE ---
    ESP_LOGI("main", "Initializing ESP-IDF networking stack...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI("main", "ESP-IDF networking initialized");
    // --------------------------------------------------
    
    // Start supervisor with our services
    supervisor_start(services);
    
    // Give supervisor time to start
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    ESP_LOGI("main", "Bootloader exiting, supervisor in control");
    
    // Bootloader task can delete itself
    vTaskDelete(NULL);
}