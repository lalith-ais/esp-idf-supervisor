#ifndef PRIORITIES_H
#define PRIORITIES_H

/*
 * priorities.h - Centralised FreeRTOS task priority definitions
 *
 * Hierarchy (higher number = higher priority):
 *
 *   24  supervisor (init)        - must be highest of all managed tasks
 *   23  ethernet_supervisor
 *   22  eth-service (inner)
 *   20  mqtt_supervisor
 *   19  mqtt-service (inner)
 *   10  ds18b20_temp_supervisor
 *    5  ds18b20-temp-task (inner)
 *    5  mqtt-publish (inner)
 *    4  display-service (inner)  - low priority, purely I/O bound
 */

/* Supervisor (must be > all service supervisors) */
#define PRIO_SUPERVISOR             24

/* Ethernet layer */
#define PRIO_ETH_SUPERVISOR         23
#define PRIO_ETH_SERVICE            22

/* MQTT layer */
#define PRIO_MQTT_SUPERVISOR        20
#define PRIO_MQTT_SERVICE           19
#define PRIO_MQTT_PUBLISH           5

/* DS18B20 temperature layer */
#define PRIO_DS18B20_SUPERVISOR     10
#define PRIO_DS18B20_SERVICE        5

/* display service  -- low priority, purely I/O bound via bit-bang */
#define PRIO_DISPLAY_SERVICE        4

#endif /* PRIORITIES_H */
