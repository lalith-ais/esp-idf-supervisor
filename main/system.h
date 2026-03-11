/*
 * system.h - Service registry and supervisor task declarations
 *
 * FIXES vs original:
 *   - All function *definitions* (ethernet_supervisor, mqtt_supervisor,
 *     ds18b20_temp_supervisor) moved to system.c.  Only forward declarations
 *     remain here → including this header from multiple TUs is safe.
 *   - services[] array definition moved to system.c (declared extern here).
 *   - Unused / broken DEFINE_SERVICE_SUPERVISOR macro removed.
 *   - Priority values now come from priorities.h instead of magic numbers.
 *   - Typos in comments fixed.
 */

#ifndef SYSTEM_H
#define SYSTEM_H

#include "supervisor.h"
#include "ethernet_service.h"
#include "mqtt_service.h"
#include "ds18b20_temp.h"
#include "display_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Supervisor task entry points (defined in system.c) */
void ethernet_supervisor(void *arg);
void mqtt_supervisor(void *arg);
void ds18b20_temp_supervisor(void *arg);
void display_supervisor(void *arg);

/* NULL-terminated service registry (defined in system.c) */
extern const service_def_t services[];

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_H */
