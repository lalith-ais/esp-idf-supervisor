/*
 * supervisor.h - ESP-IDF v5.x  (v1.2 — hardened)
 *
 * HARDENING changes vs v1.1:
 *  - service_def_t gains optional heartbeat_timeout_s field.
 *    When non-zero the supervisor checks that the service's heartbeat
 *    counter is incrementing each poll interval; a stuck (but alive)
 *    task is treated identically to a dead one.
 *  - supervisor_heartbeat() and supervisor_get_last_crash() added to
 *    public API.
 *  - NVS crash-reason persistence before esp_restart().
 */

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "priorities.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

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
#define SUPERVISOR_PRIORITY PRIO_SUPERVISOR
#endif

#ifndef SUPERVISOR_TASK_NAME
#define SUPERVISOR_TASK_NAME "init"
#endif

/* NVS namespace / key used to persist crash reason across reboots */
#ifndef SUPERVISOR_NVS_NAMESPACE
#define SUPERVISOR_NVS_NAMESPACE "supervisor"
#endif

#ifndef SUPERVISOR_NVS_KEY_CRASH
#define SUPERVISOR_NVS_KEY_CRASH "last_crash"
#endif

/* =========================================================================
 * Public types
 * ========================================================================= */

typedef enum {
    RESTART_NEVER = 0,
    RESTART_ALWAYS,
    RESTART_ON_CRASH
} restart_policy_t;

typedef struct {
    const char        *name;
    void             (*entry)(void *);
    uint16_t          stack_size;
    uint8_t           priority;       /* Must be < SUPERVISOR_PRIORITY        */
    restart_policy_t  restart;
    bool              essential;      /* true -> esp_restart() if unrecoverable*/
    void             *context;

    /*
     * heartbeat_timeout_s -- optional stuck-task detection.
     *
     * Set to 0 to disable (default).
     * When non-zero: the service task must call supervisor_heartbeat(name)
     * at least once every heartbeat_timeout_s seconds.  If it does not, the
     * supervisor treats it as dead and applies the restart policy.
     *
     * Recommended: 2-3x the longest normal blocking interval of the task.
     * e.g. if the task blocks 5 s on a queue receive, use 15.
     */
    uint16_t          heartbeat_timeout_s;
} service_def_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Spawn the supervisor task and start all services in the
 *        NULL-terminated services array.
 */
void supervisor_start(const service_def_t *services);

/**
 * @brief Returns true if every essential service is alive and not stuck.
 */
bool supervisor_is_healthy(void);

/**
 * @brief Called by a service task to report liveness to the supervisor.
 *
 * Must be called at least once per heartbeat_timeout_s seconds for any
 * service that has heartbeat_timeout_s > 0 in its service_def_t.
 * Safe to call from any task -- uses an atomic counter internally.
 *
 * @param name  The service name as registered in service_def_t.name.
 */
void supervisor_heartbeat(const char *name);

/**
 * @brief Returns the name of the last essential service that caused a
 *        forced reboot, read from NVS on startup.
 *        Returns NULL if no crash recorded or NVS is unavailable.
 *        The returned pointer is to a static buffer -- do not free it.
 */
const char *supervisor_get_last_crash(void);

#ifdef __cplusplus
}
#endif

#endif /* SUPERVISOR_H */
