/*
 * system.h - Service registry and supervisor task declarations
 *
 * CHANGES vs previous version:
 *  [NET] ethernet_supervisor renamed network_supervisor.
 *  [NET] ethernet_service.h include removed; consumers use network_service.h.
 */

#ifndef SYSTEM_H
#define SYSTEM_H

#include "supervisor.h"
#include "network_service.h"    /* replaces ethernet_service.h */
#include "mqtt_service.h"
#include "ds18b20_temp.h"
#include "display_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Supervisor task entry points (defined in system.c) */
void network_supervisor(void *arg);     /* was ethernet_supervisor */
void mqtt_supervisor(void *arg);
void ds18b20_temp_supervisor(void *arg);
void display_supervisor(void *arg);

/* NULL-terminated service registry (defined in system.c) */
extern const service_def_t services[];

#ifdef __cplusplus
}
#endif

#endif /* SYSTEM_H */
