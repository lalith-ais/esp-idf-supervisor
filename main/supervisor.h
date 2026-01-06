/*
 * supervisor.h - ESP-IDF v5.x compatible version
 */

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

// ============================================================================
// Configuration
// ============================================================================

#ifndef MAX_SERVICES
#define MAX_SERVICES 16
#endif

#ifndef SUPERVISOR_CHECK_MS
#define SUPERVISOR_CHECK_MS 5000
#endif

#ifndef SUPERVISOR_TAG
#define SUPERVISOR_TAG "init"
#endif

#ifndef SUPERVISOR_STACK_SIZE
#define SUPERVISOR_STACK_SIZE 4096
#endif

#ifndef SUPERVISOR_PRIORITY
#define SUPERVISOR_PRIORITY 24
#endif

#ifndef SUPERVISOR_TASK_NAME
#define SUPERVISOR_TASK_NAME "init"
#endif

// ============================================================================
// Public Types
// ============================================================================

typedef enum {
    RESTART_NEVER = 0,
    RESTART_ALWAYS,
    RESTART_ON_CRASH
} restart_policy_t;

typedef struct {
    const char* name;
    void (*entry)(void*);
    uint16_t stack_size;
    uint8_t priority;
    restart_policy_t restart;
    bool essential;
    void* context;
} service_def_t;

// ============================================================================
// Public API
// ============================================================================

void supervisor_start(const service_def_t* services);
bool supervisor_is_healthy(void);

// ============================================================================
// Implementation
// ============================================================================

typedef struct service_t service_t;

struct service_t {
    TaskHandle_t handle;
    const service_def_t* def;
    uint8_t crash_count;
    uint32_t last_start;
    bool is_running;
};

static service_t service_table[MAX_SERVICES] = {0};
static uint8_t service_count = 0;

// ----------------------------------------------------------------------------
// Helper Functions (SIMPLIFIED - removed problematic debug code)
// ----------------------------------------------------------------------------

static const char* task_state_to_string(eTaskState state) {
    switch(state) {
        case eRunning: return "RUNNING";
        case eReady: return "READY";
        case eBlocked: return "BLOCKED";
        case eSuspended: return "SUSPENDED";
        case eDeleted: return "DELETED";
        case eInvalid: return "INVALID";
        default: return "UNKNOWN";
    }
}

// SIMPLIFIED debug function without TaskStatus_t issues
static void print_simple_debug(void) {
    ESP_LOGI("debug", "=== SYSTEM DEBUG ===");
    ESP_LOGI("debug", "Heap free: %"PRIu32, esp_get_free_heap_size());
    ESP_LOGI("debug", "Services registered: %d", service_count);
    
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (service_table[i].def != NULL) {
            const char* state_str = "UNKNOWN";
            if (service_table[i].handle != NULL) {
                eTaskState state = eTaskGetState(service_table[i].handle);
                state_str = task_state_to_string(state);
            }
            ESP_LOGI("debug", "Service %d: %s (%s, crashes: %d)", 
                    i, service_table[i].def->name, state_str, 
                    service_table[i].crash_count);
        }
    }
}

// ----------------------------------------------------------------------------
// Core Functions
// ----------------------------------------------------------------------------


// In supervisor.h, update start_service:
static void start_service(const service_def_t* def) {
    ESP_LOGI(SUPERVISOR_TAG, "Starting service: %s", def->name);
    
    // Check priority
    if (def->priority >= SUPERVISOR_PRIORITY) {
        ESP_LOGE(SUPERVISOR_TAG, "ERROR: Service %s priority %d >= supervisor %d",
                def->name, def->priority, SUPERVISOR_PRIORITY);
        return;
    }
    
    // FIRST: Try to find existing slot for this service
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (service_table[i].def != NULL && 
            strcmp(service_table[i].def->name, def->name) == 0) {
            // Found existing slot - REUSE IT
            ESP_LOGI(SUPERVISOR_TAG, "Reusing slot %d for %s", i, def->name);
            
            // Clean up old task if it exists
            if (service_table[i].handle != NULL) {
                vTaskDelete(service_table[i].handle);
                vTaskDelay(10 / portTICK_PERIOD_MS); // Let it die
            }
            
            // Update the slot
            service_table[i].def = def;
            service_table[i].last_start = xTaskGetTickCount();
            // KEEP the crash_count for exponential backoff!
            
            // Create new task
            BaseType_t result = xTaskCreate(
                def->entry,
                def->name,
                def->stack_size,
                def->context,
                def->priority,
                &service_table[i].handle
            );
            
            if (result == pdPASS) {
                service_table[i].is_running = true;
                ESP_LOGI(SUPERVISOR_TAG, "RESTARTED %s (crash %d)", 
                        def->name, service_table[i].crash_count);
                
                vTaskDelay(10 / portTICK_PERIOD_MS);
                if (service_table[i].handle != NULL) {
                    eTaskState state = eTaskGetState(service_table[i].handle);
                    ESP_LOGI(SUPERVISOR_TAG, "Service %s state: %s", 
                            def->name, task_state_to_string(state));
                }
            } else {
                ESP_LOGE(SUPERVISOR_TAG, "FAILED to restart %s", def->name);
                service_table[i].def = NULL;
                service_count--;
            }
            return;
        }
    }
    
    // SECOND: Find empty slot for NEW service
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (service_table[i].def == NULL) {
            service_table[i].def = def;
            service_table[i].last_start = xTaskGetTickCount();
            service_table[i].crash_count = 0;
            
            // Create the task
            BaseType_t result = xTaskCreate(
                def->entry,
                def->name,
                def->stack_size,
                def->context,
                def->priority,
                &service_table[i].handle
            );
            
            if (result == pdPASS) {
                service_table[i].is_running = true;
                service_count++;
                ESP_LOGI(SUPERVISOR_TAG, "STARTED NEW %s", def->name);
                
                vTaskDelay(10 / portTICK_PERIOD_MS);
                if (service_table[i].handle != NULL) {
                    eTaskState state = eTaskGetState(service_table[i].handle);
                    ESP_LOGI(SUPERVISOR_TAG, "Service %s state: %s", 
                            def->name, task_state_to_string(state));
                }
            } else {
                ESP_LOGE(SUPERVISOR_TAG, "FAILED to start %s", def->name);
                service_table[i].def = NULL;
            }
            return;
        }
    }
    ESP_LOGE(SUPERVISOR_TAG, "ERROR: No slot for %s", def->name);
}

static bool is_alive(service_t* svc) {
    if (svc->handle == NULL) {
        return false;
    }
    
    eTaskState state = eTaskGetState(svc->handle);
    bool alive = (state == eRunning || state == eBlocked || state == eSuspended);
    svc->is_running = alive;
    
    if (!alive) {
        ESP_LOGI(SUPERVISOR_TAG, "Service %s state: %s", 
                svc->def->name, task_state_to_string(state));
    }
    
    return alive;
}

static void handle_service_death(service_t* svc) {
    if (svc->def == NULL) return;
    
    ESP_LOGW(SUPERVISOR_TAG, "%s died (crash %d)", svc->def->name, svc->crash_count + 1);
    svc->crash_count++;
    
    // Clear handle but KEEP the slot for restart
    svc->handle = NULL;
    svc->is_running = false;
    
    // Apply restart policy
    bool restart = false;
    switch (svc->def->restart) {
        case RESTART_ALWAYS:
            restart = true;
            break;
        case RESTART_ON_CRASH:
            restart = (svc->crash_count <= 3);
            break;
        case RESTART_NEVER:
            restart = false;
            break;
    }
    
    if (restart) {
        uint32_t backoff_ms = 1000 * (1 << (svc->crash_count - 1));
        if (backoff_ms > 8000) backoff_ms = 8000;
        
        ESP_LOGI(SUPERVISOR_TAG, "Will restart %s in %"PRIu32"ms", 
                svc->def->name, backoff_ms);
        
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        start_service(svc->def);  // This will reuse the slot
    } else if (svc->def->essential) {
        ESP_LOGE(SUPERVISOR_TAG, "ESSENTIAL SERVICE %s DEAD - SYSTEM REBOOT", 
                svc->def->name);
        esp_restart();
    } else {
        ESP_LOGI(SUPERVISOR_TAG, "%s will not be restarted", svc->def->name);
        // Clear the slot completely
        svc->def = NULL;
        svc->crash_count = 0;
        service_count--;
    }
}

static void supervisor_main(void* arg) {
    const service_def_t* defs = (const service_def_t*)arg;
    
    ESP_LOGI(SUPERVISOR_TAG, "========================================");
    ESP_LOGI(SUPERVISOR_TAG, "INIT PROCESS STARTING (Priority: %d)", 
            SUPERVISOR_PRIORITY);
    ESP_LOGI(SUPERVISOR_TAG, "========================================");
    
    // Count services
    int service_count_total = 0;
    while (defs[service_count_total].name != NULL) {
        service_count_total++;
    }
    ESP_LOGI(SUPERVISOR_TAG, "Found %d services to start", service_count_total);
    
    // Start all services
    for (int i = 0; defs[i].name != NULL; i++) {
        ESP_LOGI(SUPERVISOR_TAG, "Starting service %d/%d: %s", 
                i+1, service_count_total, defs[i].name);
        start_service(&defs[i]);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    
    // Initial debug print
    print_simple_debug();
    
    ESP_LOGI(SUPERVISOR_TAG, "All services started. Entering supervision loop...");
    
    // Main supervision loop
    uint32_t loop_count = 0;
    while (1) {
        loop_count++;
        
        // Check services
        bool any_dead = false;
        for (int i = 0; i < MAX_SERVICES; i++) {
            if (service_table[i].def != NULL && !is_alive(&service_table[i])) {
                any_dead = true;
                ESP_LOGI(SUPERVISOR_TAG, "Found dead service: %s", 
                        service_table[i].def->name);
                handle_service_death(&service_table[i]);
            }
        }
        
        // Periodic debug
        if (loop_count % 6 == 0 || any_dead) { // Every ~30 seconds or if dead
            print_simple_debug();
        }
        
        vTaskDelay(SUPERVISOR_CHECK_MS / portTICK_PERIOD_MS);
    }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

void supervisor_start(const service_def_t* services) {
    ESP_LOGI("boot", "Supervisor starting...");
    
    // Verify services array
    if (services == NULL) {
        ESP_LOGE("boot", "ERROR: services array is NULL!");
        return;
    }
    
    if (services[0].name == NULL) {
        ESP_LOGE("boot", "ERROR: services array is empty!");
        return;
    }
    
    BaseType_t result = xTaskCreate(
        supervisor_main,
        SUPERVISOR_TASK_NAME,
        SUPERVISOR_STACK_SIZE,
        (void*)services,
        SUPERVISOR_PRIORITY,
        NULL
    );
    
    if (result == pdPASS) {
        ESP_LOGI("boot", "Supervisor task created");
    } else {
        ESP_LOGE("boot", "FAILED to create supervisor task: %d", result);
    }
}

bool supervisor_is_healthy(void) {
    bool healthy = true;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (service_table[i].def != NULL && service_table[i].def->essential) {
            if (!is_alive(&service_table[i])) {
                ESP_LOGE(SUPERVISOR_TAG, "Essential service %s is dead!", 
                        service_table[i].def->name);
                healthy = false;
            }
        }
    }
    return healthy;
}

#ifdef __cplusplus
}
#endif

#endif // SUPERVISOR_H
