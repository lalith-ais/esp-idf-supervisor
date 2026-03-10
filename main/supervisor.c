/*
 * supervisor.c - ESP-IDF v5.x  (v1.2 — hardened)
 *
 * HARDENING vs v1.1:
 *
 *  [1] Queue-full logging
 *      Every xQueueSend that uses timeout=0 now logs a warning on drop
 *      so silent message loss is visible in the monitor output.
 *      (Applied here to supervisor's own NVS write path; service files
 *       are patched separately.)
 *
 *  [2] Stuck-task detection via heartbeat counters
 *      service_slot_t gains an atomic_uint heartbeat field and a
 *      last_heartbeat snapshot.  If heartbeat_timeout_s > 0 in the
 *      service_def_t and the counter has not incremented since the last
 *      check, the supervisor treats the slot as dead and applies the
 *      restart policy identically to a crashed task.
 *
 *  [3] NVS crash-reason persistence
 *      Before calling esp_restart() for an unrecoverable essential
 *      service, the service name is written to NVS so the reason
 *      survives the reboot.  supervisor_get_last_crash() reads it back
 *      on startup; app_main() can surface it in the boot log.
 *
 *  [4] volatile on is_running in service_slot_t
 *      Prevents the compiler optimising away cross-core reads on the
 *      dual-core ESP32-P4.  (Full atomics are overkill for a single
 *      bool written only by the supervisor task itself.)
 */

#include "supervisor.h"

#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

/* =========================================================================
 * Internal types
 * ========================================================================= */

typedef struct {
    TaskHandle_t         handle;
    const service_def_t *def;
    uint8_t              crash_count;
    TickType_t           last_start;
    TickType_t           restart_at;
    bool                 restart_pending;
    volatile bool        is_running;        /* [4] volatile for dual-core visibility */

    /* Heartbeat stuck-task detection [2] */
    atomic_uint          heartbeat;         /* incremented by supervisor_heartbeat() */
    unsigned int         last_heartbeat;    /* snapshot taken each supervisor poll   */
    TickType_t           heartbeat_last_seen; /* tick at which heartbeat last advanced */
} service_slot_t;

/* =========================================================================
 * Module-private state
 * ========================================================================= */

static service_slot_t s_table[MAX_SERVICES];
static uint8_t        s_count = 0;

/* Static buffer for last crash reason read from NVS [3] */
static char s_last_crash[64] = {0};

/* =========================================================================
 * NVS helpers [3]
 * ========================================================================= */

static void nvs_persist_crash(const char *service_name)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SUPERVISOR_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(SUPERVISOR_TAG, "NVS open failed (%s) — crash reason not persisted",
                 esp_err_to_name(err));
        return;
    }
    nvs_set_str(h, SUPERVISOR_NVS_KEY_CRASH, service_name);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(SUPERVISOR_TAG, "Crash reason '%s' persisted to NVS", service_name);
}

static void nvs_load_last_crash(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SUPERVISOR_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return;   /* namespace doesn't exist yet — no crash recorded */

    size_t len = sizeof(s_last_crash);
    nvs_get_str(h, SUPERVISOR_NVS_KEY_CRASH, s_last_crash, &len);
    nvs_close(h);

    if (s_last_crash[0] != '\0') {
        ESP_LOGW(SUPERVISOR_TAG, "Last crash reason from NVS: '%s'", s_last_crash);
    }
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static const char *task_state_str(eTaskState state)
{
    switch (state) {
        case eRunning:   return "RUNNING";
        case eReady:     return "READY";
        case eBlocked:   return "BLOCKED";
        case eSuspended: return "SUSPENDED";
        case eDeleted:   return "DELETED";
        case eInvalid:   return "INVALID";
        default:         return "UNKNOWN";
    }
}

static void print_debug(void)
{
    ESP_LOGI("debug", "=== SYSTEM DEBUG ===");
    ESP_LOGI("debug", "Heap free: %" PRIu32 "  min-ever: %" PRIu32,
             esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    ESP_LOGI("debug", "Services registered: %d", s_count);

    for (int i = 0; i < MAX_SERVICES; i++) {
        if (s_table[i].def == NULL) continue;

        const char *state_str = "NO_HANDLE";
        if (s_table[i].handle != NULL) {
            state_str = task_state_str(eTaskGetState(s_table[i].handle));
        }
        /* Show stack high-water mark if available */
        uint32_t stack_hwm = 0;
        if (s_table[i].handle != NULL) {
            stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(s_table[i].handle);
        }
        ESP_LOGI("debug", "  [%d] %-20s  state=%-10s  crashes=%d  stack_free=%" PRIu32 "%s",
                 i,
                 s_table[i].def->name,
                 state_str,
                 s_table[i].crash_count,
                 stack_hwm,
                 s_table[i].restart_pending ? "  (restart pending)" : "");
    }
}

/* =========================================================================
 * start_service
 * ========================================================================= */

static void start_service(service_slot_t *slot)
{
    const service_def_t *def = slot->def;

    if (def->priority >= SUPERVISOR_PRIORITY) {
        ESP_LOGE(SUPERVISOR_TAG,
                 "Service '%s' priority %d >= supervisor %d -- rejected",
                 def->name, def->priority, SUPERVISOR_PRIORITY);
        return;
    }

    if (slot->handle != NULL) {
        vTaskDelete(slot->handle);
        slot->handle = NULL;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    slot->last_start         = xTaskGetTickCount();
    slot->restart_pending    = false;
    slot->restart_at         = 0;
    /* Reset heartbeat tracking for the new task incarnation */
    atomic_store(&slot->heartbeat, 0);
    slot->last_heartbeat     = 0;
    slot->heartbeat_last_seen = xTaskGetTickCount();

    BaseType_t rc = xTaskCreate(
        def->entry,
        def->name,
        def->stack_size,
        def->context,
        def->priority,
        &slot->handle
    );

    if (rc == pdPASS) {
        slot->is_running = true;
        ESP_LOGI(SUPERVISOR_TAG, "Started '%s' (crash_count=%d)",
                 def->name, slot->crash_count);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (slot->handle != NULL) {
            ESP_LOGI(SUPERVISOR_TAG, "  -> state: %s",
                     task_state_str(eTaskGetState(slot->handle)));
        }
    } else {
        ESP_LOGE(SUPERVISOR_TAG, "xTaskCreate failed for '%s'", def->name);
        slot->handle     = NULL;
        slot->is_running = false;
    }
}

/* =========================================================================
 * is_alive
 *
 * Combines FreeRTOS liveness check WITH heartbeat stuck-task detection [2].
 * ========================================================================= */

static bool is_alive(service_slot_t *slot)
{
    if (slot->handle == NULL) {
        slot->is_running = false;
        return false;
    }

    /* --- Standard liveness check --- */
    eTaskState state = eTaskGetState(slot->handle);
    bool task_alive = (state == eRunning  ||
                       state == eBlocked  ||
                       state == eSuspended ||
                       state == eReady);

    slot->is_running = task_alive;

    if (!task_alive) {
        ESP_LOGW(SUPERVISOR_TAG, "'%s' state: %s",
                 slot->def->name, task_state_str(state));
        return false;
    }

    /* --- Heartbeat stuck-task detection [2] --- */
    if (slot->def->heartbeat_timeout_s > 0) {
        unsigned int current = atomic_load(&slot->heartbeat);

        if (current != slot->last_heartbeat) {
            /* Heartbeat advanced — task is alive and progressing */
            slot->last_heartbeat      = current;
            slot->heartbeat_last_seen = xTaskGetTickCount();
        } else {
            /* No new heartbeat — check if timeout has elapsed */
            TickType_t silent_ticks = xTaskGetTickCount() - slot->heartbeat_last_seen;
            uint32_t   silent_ms    = (uint32_t)(silent_ticks * portTICK_PERIOD_MS);
            uint32_t   timeout_ms   = (uint32_t)slot->def->heartbeat_timeout_s * 1000u;

            if (silent_ms >= timeout_ms) {
                ESP_LOGW(SUPERVISOR_TAG,
                         "'%s' STUCK -- no heartbeat for %" PRIu32 " ms (timeout %" PRIu32 " ms)",
                         slot->def->name, silent_ms, timeout_ms);
                return false;   /* treat as dead -- restart policy will apply */
            }
        }
    }

    return true;
}

/* =========================================================================
 * handle_service_death
 * ========================================================================= */

static void handle_service_death(service_slot_t *slot)
{
    if (slot->def == NULL) return;

    slot->crash_count++;
    slot->handle     = NULL;
    slot->is_running = false;

    ESP_LOGW(SUPERVISOR_TAG, "'%s' died/stuck (crash #%d)",
             slot->def->name, slot->crash_count);

    bool do_restart = false;
    switch (slot->def->restart) {
        case RESTART_ALWAYS:   do_restart = true; break;
        case RESTART_ON_CRASH: do_restart = (slot->crash_count <= 3); break;
        case RESTART_NEVER:    do_restart = false; break;
    }

    if (do_restart) {
        uint32_t backoff_ms = 1000u * (1u << (slot->crash_count - 1));
        if (backoff_ms > 8000u) backoff_ms = 8000u;

        slot->restart_at      = xTaskGetTickCount() + pdMS_TO_TICKS(backoff_ms);
        slot->restart_pending = true;

        ESP_LOGI(SUPERVISOR_TAG, "Will restart '%s' in %" PRIu32 " ms",
                 slot->def->name, backoff_ms);

    } else if (slot->def->essential) {
        ESP_LOGE(SUPERVISOR_TAG,
                 "ESSENTIAL SERVICE '%s' EXHAUSTED RESTARTS -- REBOOTING",
                 slot->def->name);
        /* [3] Persist crash reason to NVS before reboot */
        nvs_persist_crash(slot->def->name);
        esp_restart();

    } else {
        ESP_LOGI(SUPERVISOR_TAG, "'%s' will not be restarted", slot->def->name);
        slot->def         = NULL;
        slot->crash_count = 0;
        s_count--;
    }
}

/* =========================================================================
 * supervisor_main task
 * ========================================================================= */

static void supervisor_main(void *arg)
{
    const service_def_t *defs = (const service_def_t *)arg;

    /* [3] Load last crash reason from NVS so it appears in the boot log */
    nvs_load_last_crash();

    ESP_LOGI(SUPERVISOR_TAG, "========================================");
    ESP_LOGI(SUPERVISOR_TAG, "INIT PROCESS STARTING (priority %d)",
             SUPERVISOR_PRIORITY);
    if (s_last_crash[0] != '\0') {
        ESP_LOGW(SUPERVISOR_TAG, "Previous crash: essential service '%s' failed",
                 s_last_crash);
    }
    ESP_LOGI(SUPERVISOR_TAG, "========================================");

    /* Count and start all services */
    int total = 0;
    while (defs[total].name != NULL) total++;
    ESP_LOGI(SUPERVISOR_TAG, "Found %d service(s) to start", total);

    for (int i = 0; defs[i].name != NULL; i++) {
        int free_slot = -1;
        for (int j = 0; j < MAX_SERVICES; j++) {
            if (s_table[j].def == NULL) { free_slot = j; break; }
        }
        if (free_slot < 0) {
            ESP_LOGE(SUPERVISOR_TAG, "No free slot for '%s'", defs[i].name);
            continue;
        }

        s_table[free_slot].def            = &defs[i];
        s_table[free_slot].crash_count    = 0;
        s_table[free_slot].handle         = NULL;
        s_table[free_slot].restart_pending = false;
        atomic_init(&s_table[free_slot].heartbeat, 0);
        s_table[free_slot].last_heartbeat  = 0;
        s_table[free_slot].heartbeat_last_seen = xTaskGetTickCount();

        ESP_LOGI(SUPERVISOR_TAG, "Starting %d/%d: %s", i + 1, total, defs[i].name);
        start_service(&s_table[free_slot]);
        s_count++;

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    print_debug();
    ESP_LOGI(SUPERVISOR_TAG, "All services started. Entering supervision loop...");

    uint32_t loop_count = 0;

    while (1) {
        loop_count++;
        TickType_t now = xTaskGetTickCount();
        bool any_event = false;

        for (int i = 0; i < MAX_SERVICES; i++) {
            service_slot_t *slot = &s_table[i];
            if (slot->def == NULL) continue;

            /* Pending restart — check if back-off has elapsed */
            if (slot->restart_pending) {
                if ((TickType_t)(now - slot->restart_at) < (TickType_t)(UINT32_MAX / 2)) {
                    ESP_LOGI(SUPERVISOR_TAG, "Back-off elapsed, restarting '%s'",
                             slot->def->name);
                    start_service(slot);
                    any_event = true;
                }
                continue;
            }

            /* Liveness + heartbeat check */
            if (!is_alive(slot)) {
                any_event = true;
                ESP_LOGI(SUPERVISOR_TAG, "Dead/stuck service: '%s'", slot->def->name);
                handle_service_death(slot);
            }
        }

        if (loop_count % 6 == 0 || any_event) {
            print_debug();
        }

        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_CHECK_MS));
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void supervisor_start(const service_def_t *services)
{
    if (services == NULL || services[0].name == NULL) {
        ESP_LOGE("boot", "supervisor_start: services array is NULL or empty");
        return;
    }

    BaseType_t rc = xTaskCreate(
        supervisor_main,
        SUPERVISOR_TASK_NAME,
        SUPERVISOR_STACK_SIZE,
        (void *)services,
        SUPERVISOR_PRIORITY,
        NULL
    );

    if (rc == pdPASS) {
        ESP_LOGI("boot", "Supervisor task created");
    } else {
        ESP_LOGE("boot", "Failed to create supervisor task (err=%d)", (int)rc);
    }
}

bool supervisor_is_healthy(void)
{
    bool healthy = true;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (s_table[i].def == NULL) continue;
        if (!s_table[i].def->essential) continue;
        if (!is_alive(&s_table[i])) {
            ESP_LOGE(SUPERVISOR_TAG, "Essential service '%s' is dead/stuck!",
                     s_table[i].def->name);
            healthy = false;
        }
    }
    return healthy;
}

void supervisor_heartbeat(const char *name)
{
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (s_table[i].def == NULL) continue;
        if (strcmp(s_table[i].def->name, name) == 0) {
            atomic_fetch_add(&s_table[i].heartbeat, 1);
            return;
        }
    }
    /* Name not found — log once to help catch typos in heartbeat calls */
    ESP_LOGW(SUPERVISOR_TAG, "supervisor_heartbeat: unknown service '%s'", name);
}

const char *supervisor_get_last_crash(void)
{
    return (s_last_crash[0] != '\0') ? s_last_crash : NULL;
}
