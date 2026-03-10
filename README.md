# ESP32-P4 Supervisor

> A FreeRTOS service supervision framework for ESP-IDF v5.x — start, monitor, and automatically restart application tasks, inspired by Unix `init` / `systemd`.

Tested on real hardware: **ESP32-P4** with internal Ethernet (IP101 PHY), MQTT broker, and DS18B20 temperature sensors.

---

## Table of Contents

- [Features](#features)
- [Design Philosophy](#design-philosophy)
- [Architecture](#architecture)
- [File Structure](#file-structure)
- [Quick Start](#quick-start)
- [Task Priority Hierarchy](#task-priority-hierarchy)
- [Supervisor](#supervisor)
  - [Configuration Macros](#configuration-macros)
  - [Restart Policies](#restart-policies)
  - [Exponential Back-off](#exponential-back-off)
  - [Public API](#supervisor-public-api)
- [Services](#services)
  - [Ethernet Service](#ethernet-service)
  - [MQTT Service](#mqtt-service)
  - [DS18B20 Temperature Service](#ds18b20-temperature-service)
- [Adding a New Service](#adding-a-new-service)
- [Build & Configuration](#build--configuration)
- [Hardware Pin Assignment](#hardware-pin-assignment)
- [Known Issues & Limitations](#known-issues--limitations)
- [Changelog](#changelog)
- [License](#license)

---

## Features

- **Service supervision** — monitors FreeRTOS tasks and restarts them on failure
- **Configurable restart policies** — `RESTART_NEVER`, `RESTART_ALWAYS`, `RESTART_ON_CRASH`
- **Non-blocking exponential back-off** — failed restarts don't stall the supervisor loop
- **Essential service support** — triggers `esp_restart()` if a critical service can't recover
- **Two-tier supervision model** — supervisor → wrapper task → inner service task
- **Ethernet** with event-driven state machine and IP tracking
- **MQTT** publish/subscribe with automatic reconnection on network loss
- **DS18B20** 1-Wire temperature sensing with typed event queue
- **Centralised priority definitions** — no magic numbers anywhere
- **ESP-IDF v5.x compatible**

---

## Design Philosophy

This codebase deliberately avoids mutexes, semaphores, and shared memory. Every piece of state is **owned by exactly one task**. All inter-task communication happens exclusively through **FreeRTOS queues**.

This follows the [Actor model](https://en.wikipedia.org/wiki/Actor_model) — the same principle behind Erlang's processes and Go's channels:

> *"Don't communicate by sharing memory; share memory by communicating."*

The practical benefits in an embedded context are significant:

- **No priority inversion** — a common and dangerous bug in embedded systems where a high-priority task is blocked by a low-priority one holding a mutex
- **No deadlock possible** — you cannot deadlock with queues the way you can with mutexes
- **Deterministic behaviour** — each task's state is self-contained and easy to reason about
- **Clean crash isolation** — a failed task's owned resources are well-defined; the supervisor can restart it without needing to unpick shared state
- **Readable stack traces** — a crashed task tells you exactly where it failed, with no ambiguity about lock ownership

### Rules for contributors

If you are adding a new service, please follow the same pattern:

- ✅ **Do** own all service state in a single `static` context struct inside your `.c` file
- ✅ **Do** expose state to other tasks via queue messages or getter functions
- ✅ **Do** use `xQueueSend()` to signal shutdown from outside your task
- ❌ **Don't** use `SemaphoreHandle_t`, `MutexHandle_t`, or `vTaskSuspend()`
- ❌ **Don't** access another service's internal state directly

A shutdown request from outside a task looks like this — no semaphore needed:

```c
// Correct: send a message, let the task clean up its own resources
my_service_message_t msg = { .type = MY_EVENT_STOP_REQUESTED };
xQueueSend(my_ctx.event_queue, &msg, pdMS_TO_TICKS(100));
```

The task receives it in its own event loop, cleans up its owned resources, and calls `vTaskDelete(NULL)` — fully in control of its own teardown.

---

## Architecture

The firmware uses a two-tier supervision hierarchy. A single top-level supervisor task (priority 24) owns a table of service wrapper tasks. Each wrapper manages one inner service task and forwards events to a queue.

```
app_main  (main.c)
  └─ supervisor_start(services)          ← supervisor.c
        ├─ ethernet_supervisor           ← system.c   [priority 23]
        │    └─ ethernet_service_task    ← ethernet_service.c   [priority 22]
        │         └─ ethernet_setup      ← ethernet_setup.c  (HW driver)
        │
        ├─ mqtt_supervisor               ← system.c   [priority 20]
        │    ├─ mqtt_service_task        ← mqtt_service.c   [priority 19]
        │    └─ mqtt_publish_task        ← mqtt_service.c   [priority  5]
        │         └─ app_mqtt            ← app_mqtt.c  (ESP-IDF MQTT wrapper)
        │
        └─ ds18b20_temp_supervisor       ← system.c   [priority 10]
             └─ ds18b20_temp_task        ← ds18b20_temp.c   [priority  5]
                   └─ 1-Wire RMT bus     (espressif__onewire_bus component)
```

---

## File Structure

```
esp-idf-supervisor/
├── CMakeLists.txt
└── main/
    ├── CMakeLists.txt
    ├── main.c                  # Boot entry point
    ├── priorities.h            # All FreeRTOS priority constants (single source of truth)
    ├── supervisor.h            # Public supervisor API + types
    ├── supervisor.c            # Supervisor implementation
    ├── system.h                # Service supervisor declarations + extern services[]
    ├── system.c                # Supervisor task bodies + service registry
    ├── ethernet_service.h/.c   # Ethernet wrapper — event queue, state, IP tracking
    ├── ethernet_setup.h/.c     # Low-level ESP-IDF Ethernet driver
    ├── mqtt_service.h/.c       # MQTT wrapper — broker lifecycle, pub/sub
    ├── app_mqtt.h/.c           # Thin ESP-IDF MQTT client wrapper
    └── ds18b20_temp.h/.c       # DS18B20 1-Wire temperature service
```

---

## Quick Start

### 1. Clone and set up the IDF environment

```bash
git clone https://github.com/lalith-ais/esp-idf-supervisor.git
cd esp-idf-supervisor
idf.py set-target esp32p4
```

### 2. Configure

```bash
idf.py menuconfig
```

Set at minimum:

| Key | Example Value |
|-----|---------------|
| `CONFIG_MQTT_BROKER_URI` | `mqtt://192.168.1.100` |
| `CONFIG_ESP_TASK_WDT` | `y` |
| `CONFIG_FREERTOS_HZ` | `1000` |

> **Warning:** `CONFIG_MQTT_BROKER_URI` **must** be set. If absent the build will warn and use a placeholder that will fail to connect.

### 3. Build and flash

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### 4. Expected boot log

```

I (25) boot: ESP-IDF v5.5.2 2nd stage bootloader
I (26) boot: compile time Mar 10 2026 08:28:45
I (26) boot: Multicore bootloader
I (27) boot: chip revision: v1.3
I (29) boot: efuse block revision: v0.3
I (32) boot.esp32p4: SPI Speed      : 80MHz
I (36) boot.esp32p4: SPI Mode       : DIO
I (40) boot.esp32p4: SPI Flash Size : 32MB
I (44) boot: Enabling RNG early entropy source...
I (48) boot: Partition Table:
I (51) boot: ## Label            Usage          Type ST Offset   Length
I (57) boot:  0 nvs              WiFi data        01 02 00009000 00006000
I (64) boot:  1 phy_init         RF data          01 01 0000f000 00001000
I (70) boot:  2 factory          factory app      00 00 00010000 00100000
I (78) boot: End of partition table
I (80) esp_image: segment 0: paddr=00010020 vaddr=40080020 size=200c8h (131272) map
I (110) esp_image: segment 1: paddr=000300f0 vaddr=30100000 size=00088h (   136) load
I (112) esp_image: segment 2: paddr=00030180 vaddr=4ff00000 size=0fe98h ( 65176) load
I (128) esp_image: segment 3: paddr=00040020 vaddr=40000020 size=7dd70h (515440) map
I (215) esp_image: segment 4: paddr=000bdd98 vaddr=4ff0fe98 size=04c08h ( 19464) load
I (221) esp_image: segment 5: paddr=000c29a8 vaddr=4ff14b00 size=02dd4h ( 11732) load
I (229) boot: Loaded app from partition at offset 0x10000
I (229) boot: Disabling RNG early entropy source...
I (243) hex_psram: vendor id    : 0x0d (AP)
I (243) hex_psram: Latency      : 0x01 (Fixed)
I (243) hex_psram: DriveStr.    : 0x00 (25 Ohm)
I (244) hex_psram: dev id       : 0x03 (generation 4)
I (249) hex_psram: density      : 0x07 (256 Mbit)
I (253) hex_psram: good-die     : 0x06 (Pass)
I (257) hex_psram: SRF          : 0x02 (Slow Refresh)
I (262) hex_psram: BurstType    : 0x00 ( Wrap)
I (266) hex_psram: BurstLen     : 0x03 (2048 Byte)
I (271) hex_psram: BitMode      : 0x01 (X16 Mode)
I (275) hex_psram: Readlatency  : 0x04 (14 cycles@Fixed)
I (280) hex_psram: DriveStrength: 0x00 (1/1)
I (284) MSPI Timing: Enter psram timing tuning
I (461) esp_psram: Found 32MB PSRAM device
I (462) esp_psram: Speed: 200MHz
I (462) hex_psram: psram CS IO is dedicated
I (463) cpu_start: Multicore app
I (1413) esp_psram: SPI SRAM memory test OK
I (1422) cpu_start: GPIO 38 and 37 are used as console UART I/O pins
I (1423) cpu_start: Pro cpu start user code
I (1423) cpu_start: cpu freq: 360000000 Hz
I (1425) app_init: Application information:
I (1429) app_init: Project name:     esp-idf-supervisor
I (1434) app_init: App version:      a3f94b1
I (1438) app_init: Compile time:     Mar 10 2026 08:29:00
I (1443) app_init: ELF file SHA256:  a41b5889a...
I (1447) app_init: ESP-IDF:          v5.5.2
I (1451) efuse_init: Min chip rev:     v0.1
I (1455) efuse_init: Max chip rev:     v1.99 
I (1459) efuse_init: Chip rev:         v1.3
I (1463) heap_init: Initializing. RAM available for dynamic allocation:
I (1469) heap_init: At 4FF1A640 len 00020980 (130 KiB): RETENT_RAM
I (1475) heap_init: At 4FF3AFC0 len 00004BF0 (18 KiB): RAM
I (1480) heap_init: At 4FF40000 len 00060000 (384 KiB): RAM
I (1486) heap_init: At 50108080 len 00007F80 (31 KiB): RTCRAM
I (1491) heap_init: At 30100088 len 00001F78 (7 KiB): TCM
I (1497) esp_psram: Adding pool of 32768K of PSRAM memory to heap allocator
I (1504) spi_flash: detected chip: gd
I (1506) spi_flash: flash io: dio
I (1510) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (1516) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (1523) main_task: Started on CPU0
I (1553) esp_psram: Reserving pool of 32K of internal memory for DMA/internal allocations
I (1553) main_task: Calling app_main()
I (1563) main: Bootloader starting. Heap free: 34082260
I (1563) main: Initializing ESP-IDF networking stack...
I (1563) main: ESP-IDF networking initialized
I (1563) init: ========================================
I (1573) init: INIT PROCESS STARTING (priority 24)
I (1573) init: ========================================
I (1583) init: Found 3 service(s) to start
I (1583) init: Starting 1/3: ethernet
I (1593) init: Started 'ethernet' (crash_count=0)
I (1593) ethernet-super: Starting
I (1593) boot: Supervisor task created
I (1603) init:   -> state: READY
I (1593) eth-service: Ethernet service starting
I (1613) ethernet-super: Running
I (1643) esp_eth.netif.netif_glue: 30:ed:a0:ea:53:25
I (1643) esp_eth.netif.netif_glue: ethernet attached to netif
I (1653) init: Starting 2/3: mqtt
I (1653) init: Started 'mqtt' (crash_count=0)
I (1653) mqtt-super: Starting
I (1653) mqtt-service: MQTT service starting
I (1653) mqtt-service: Waiting for Ethernet IP...
I (1663) init:   -> state: READY
I (1663) mqtt-super: Running
I (1713) init: Starting 3/3: ds18b20-temp
I (1713) init: Started 'ds18b20-temp' (crash_count=0)
I (1713) ds18b20-super: DS18B20 temperature supervisor starting
I (1713) ds18b20-temp: Initialising DS18B20 on GPIO6
I (1723) init:   -> state: RUNNING
I (1773) debug: === SYSTEM DEBUG ===
I (1773) debug: Heap free: 33989596  min-ever: 33989596
I (1773) debug: Services registered: 3
I (1773) debug:   [0] ethernet              state=BLOCKED     crashes=0  stack_free=10460
I (1773) debug:   [1] mqtt                  state=BLOCKED     crashes=0  stack_free=6092
I (1783) debug:   [2] ds18b20-temp          state=BLOCKED     crashes=0  stack_free=2200
I (1793) init: All services started. Entering supervision loop...
I (1823) ds18b20-temp: Found DS18B20[0] addr=D506473B2D600028
I (1923) ds18b20-temp: Found DS18B20[1] addr=6805473B4A510028
I (2023) ds18b20-temp: Found DS18B20[2] addr=4706473B10350028
I (2023) ds18b20-temp: Found 3 sensor(s)
I (2023) ds18b20-temp: Service started (3 sensor(s))
I (2023) ds18b20-temp: Task starting
I (2033) ds18b20-super: Running
I (2603) main: Bootloader exiting, supervisor in control
W (2663) mqtt-super: No Ethernet IP — MQTT service will handle reconnection
W (3663) mqtt-super: No Ethernet IP — MQTT service will handle reconnection
I (4443) ethernet_setup: Ethernet initialized successfully
I (4443) eth-service: Running -- waiting for events...
I (4443) ethernet_setup: Ethernet Started
I (4443) ethernet_setup: Ethernet Link Up
I (4443) ethernet_setup: MAC: 30:ed:a0:ea:53:25
W (4663) mqtt-super: No Ethernet IP — MQTT service will handle reconnection
I (5253) ds18b20-temp: Sensor[0]: 16.44°C (count=0)
I (5253) ds18b20-super: Sensor[0]: 16.44°C
I (5263) ds18b20-temp: Sensor[1]: 16.31°C (count=0)
I (5263) ds18b20-super: Sensor[1]: 16.31°C
I (5273) ds18b20-temp: Sensor[2]: 16.81°C (count=0)
I (5273) ds18b20-super: Sensor[2]: 16.81°C
I (5443) ethernet-super: Ethernet connected
I (5453) ethernet_setup: Got IP Address
I (5453) ethernet_setup: IP:      192.168.124.90
I (5453) ethernet_setup: Netmask: 255.255.255.0
I (5453) ethernet_setup: Gateway: 192.168.124.1
I (5453) ethernet-super: Got IP: 192.168.124.90
I (5453) esp_netif_handlers: eth ip: 192.168.124.90, mask: 255.255.255.0, gw: 192.168.124.1
W (5663) mqtt-super: No Ethernet IP — MQTT service will handle reconnection
I (6443) ethernet-super: Got IP: 192.168.124.90
I (6653) mqtt-service: Connecting to broker: mqtt://192.168.124.4 (LWT: /ESP32P4/NODE1/status = offline)
I (6653) app-mqtt: LWT configured: topic=/ESP32P4/NODE1/status msg=offline qos=1 retain=1
I (6653) app-mqtt: MQTT client initialised: broker=mqtt://192.168.124.4 client_id=ESP32P4-ETH
I (6663) mqtt-super: MQTT service started
I (6663) mqtt-service: Running
I (6663) mqtt-service: Health publish task started
I (6673) app-mqtt: MQTT_EVENT_CONNECTED
I (6673) mqtt-service: MQTT connected
I (6683) mqtt-service: Published: /ESP32P4/NODE1/status = online (retained)
I (6683) mqtt-super: MQTT connected
I (7673) mqtt-super: Published to /ESP32P4/NODE1/health, msg_id=53839
I (7673) mqtt-service: Health: {"uptime_s":0,"heap_free":33973708,"ip":"192.168.124.90"}
I (13513) ds18b20-temp: Sensor[0]: 16.50°C (count=0)
I (13513) ds18b20-super: Sensor[0]: 16.50°C
I (13513) ds18b20-temp: Published /ESP32P4/temperature/0 → 16.50
I (13533) ds18b20-temp: Sensor[1]: 16.38°C (count=1)
I (13533) ds18b20-super: Sensor[1]: 16.38°C
I (13533) ds18b20-temp: Published /ESP32P4/temperature/1 → 16.38
I (13543) ds18b20-temp: Sensor[2]: 16.88°C (count=2)
I (13543) ds18b20-super: Sensor[2]: 16.88°C
I (13543) ds18b20-temp: Published /ESP32P4/temperature/2 → 16.88


```

---

## Task Priority Hierarchy

Defined in `priorities.h`. **Never use numeric literals** for task priorities in application code — always use these constants.

| Constant | Value | Task |
|----------|-------|------|
| `PRIO_SUPERVISOR` | 24 | supervisor (init) — must be highest |
| `PRIO_ETH_SUPERVISOR` | 23 | `ethernet_supervisor` wrapper |
| `PRIO_ETH_SERVICE` | 22 | `eth-service` inner task |
| `PRIO_MQTT_SUPERVISOR` | 20 | `mqtt_supervisor` wrapper |
| `PRIO_MQTT_SERVICE` | 19 | `mqtt-service` inner task |
| `PRIO_DS18B20_SUPERVISOR` | 10 | `ds18b20_temp_supervisor` wrapper |
| `PRIO_MQTT_PUBLISH` | 5 | `mqtt-publish` inner task |
| `PRIO_DS18B20_SERVICE` | 5 | `ds18b20-temp` inner task |

The supervisor enforces that no service may have a priority >= `PRIO_SUPERVISOR`. Any attempt to register such a service is rejected with an error log.

---

## Supervisor

### Configuration Macros

Override any of these in `CMakeLists.txt` or via compiler flags:

| Macro | Default | Description |
|-------|---------|-------------|
| `MAX_SERVICES` | `16` | Maximum number of concurrently managed services |
| `SUPERVISOR_CHECK_MS` | `5000` | Liveness poll interval (ms) |
| `SUPERVISOR_PRIORITY` | `24` | FreeRTOS priority of the supervisor task |
| `SUPERVISOR_STACK_SIZE` | `4096` | Supervisor task stack size (bytes) |
| `SUPERVISOR_TASK_NAME` | `"init"` | FreeRTOS task name |
| `SUPERVISOR_TAG` | `"init"` | ESP_LOG tag |

### Restart Policies

Set per service in the `service_def_t` struct:

| Policy | Behaviour |
|--------|-----------|
| `RESTART_NEVER` | Task is never restarted after it exits or crashes |
| `RESTART_ALWAYS` | Task is always restarted regardless of crash count |
| `RESTART_ON_CRASH` | Task is restarted for up to 3 crashes; after that, if `essential=true` the system reboots, otherwise the slot is released |

### Exponential Back-off

Back-off is **non-blocking** — the supervisor loop continues monitoring all other services while a restart is pending.

```
back-off (ms) = min(1000 × 2^(crash_count − 1), 8000)

Crash 1 →  1 000 ms
Crash 2 →  2 000 ms
Crash 3 →  4 000 ms
Crash 4+ →  8 000 ms  (capped)
```

### Supervisor Public API

```c
// Spawn the supervisor task and start all services.
// `services` is a NULL-terminated array of service_def_t.
void supervisor_start(const service_def_t *services);

// Returns true if every service marked essential=true is currently alive.
bool supervisor_is_healthy(void);
```

#### `service_def_t` fields

```c
typedef struct {
    const char       *name;        // Task name (also used as slot key)
    void            (*entry)(void *); // Task entry function
    uint16_t          stack_size;  // Stack in bytes
    uint8_t           priority;    // Must be < PRIO_SUPERVISOR
    restart_policy_t  restart;     // RESTART_NEVER / RESTART_ALWAYS / RESTART_ON_CRASH
    bool              essential;   // If true and unrecoverable → esp_restart()
    void             *context;     // Passed as arg to entry()
} service_def_t;
```

#### Service registry example (`system.c`)

```c
const service_def_t services[] = {
    {"ethernet",     ethernet_supervisor,     12288, PRIO_ETH_SUPERVISOR,    RESTART_ALWAYS, true,  NULL},
    {"mqtt",         mqtt_supervisor,          8192, PRIO_MQTT_SUPERVISOR,   RESTART_ALWAYS, false, NULL},
    {"ds18b20-temp", ds18b20_temp_supervisor,  4096, PRIO_DS18B20_SUPERVISOR,RESTART_ALWAYS, false, NULL},
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL}  // sentinel
};
```

---

## Services

### Ethernet Service

Files: `ethernet_service.h/.c`, `ethernet_setup.h/.c`

#### Event Types

```c
typedef enum {
    ETH_EVENT_CONNECTED,     // Physical link up — MAC address available
    ETH_EVENT_DISCONNECTED,  // Physical link lost
    ETH_EVENT_GOT_IP,        // DHCP lease obtained
    ETH_EVENT_STARTED,       // Ethernet driver started
    ETH_EVENT_STOPPED,       // Ethernet driver stopped
    ETH_EVENT_ERROR          // Hardware error — wrapper task exits
} eth_event_type_t;
```

#### Public API

```c
void           ethernet_service_start(void);
void           ethernet_service_stop(void);
QueueHandle_t  ethernet_service_get_queue(void);   // Items: eth_service_message_t
bool           ethernet_service_is_connected(void);
bool           ethernet_service_has_ip(void);
const char    *ethernet_service_get_ip(void);
```

---

### MQTT Service

Files: `mqtt_service.h/.c`, `app_mqtt.h/.c`

#### sdkconfig Keys

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_MQTT_BROKER_URI` | *(required)* | Full broker URI, e.g. `mqtt://192.168.1.100` |
| `CONFIG_MQTT_CLIENT_ID` | `ESP32P4-ETH` | MQTT client identifier |
| `CONFIG_MQTT_PUBLISH_TOPIC` | `/ESP32P4/NODE1` | Outgoing telemetry topic |
| `CONFIG_MQTT_SUBSCRIBE_TOPIC` | `/ESP32P4/COMMAND` | Incoming command topic |
| `CONFIG_MQTT_PUBLISH_INTERVAL_MS` | `5000` | Publish interval (ms) |

#### Supported Commands (subscribe topic payload)

| Payload | Action |
|---------|--------|
| `led_on` | Log message (extend to drive GPIO) |
| `led_off` | Log message (extend to drive GPIO) |
| `reboot` | Log message (extend to call `esp_restart()`) |

#### Public API

```c
void           mqtt_service_start(void);
void           mqtt_service_stop(void);
bool           mqtt_service_is_running(void);
bool           mqtt_service_is_connected(void);
bool           mqtt_service_can_publish(void);      // running + connected + has IP
QueueHandle_t  mqtt_service_get_queue(void);        // Items: mqtt_service_message_t

esp_err_t      mqtt_service_publish(const char *topic, const char *data, int qos, bool retain);
esp_err_t      mqtt_service_subscribe(const char *topic, int qos);
esp_err_t      mqtt_service_unsubscribe(const char *topic);

void           mqtt_service_set_config(const mqtt_config_t *config);
void           mqtt_service_get_config(mqtt_config_t *config);
```

---

### DS18B20 Temperature Service

Files: `ds18b20_temp.h/.c`

#### Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `DS18B20_MAX_SENSORS` | `4` | Maximum sensors enumerated on the bus |
| `DS18B20_DEFAULT_GPIO` | `6` | GPIO pin for 1-Wire data line |
| `HEALTH_STALE_S` | `30` | Seconds without a reading before `is_healthy()` returns false |

#### Reading Queue

The service pushes `ds18b20_reading_t` structs onto the event queue after each successful conversion:

```c
typedef struct {
    int   sensor_index;   // 0-based sensor index
    float temperature;    // degrees Celsius
} ds18b20_reading_t;
```

#### MQTT Topics

| Topic | Condition |
|-------|-----------|
| `/ESP32P4/temperature` | Single sensor on bus |
| `/ESP32P4/temperature/N` | Multiple sensors — N is zero-based sensor index |

#### Public API

```c
void           ds18b20_temp_service_start(void);
void           ds18b20_temp_service_stop(void);
QueueHandle_t  ds18b20_temp_service_get_queue(void);        // Items: ds18b20_reading_t
bool           ds18b20_temp_service_is_healthy(void);

int            ds18b20_temp_service_get_sensor_count(void);
float          ds18b20_temp_service_get_last_temperature(int sensor_index);
uint32_t       ds18b20_temp_service_get_message_count(void);
esp_err_t      ds18b20_temp_service_trigger_conversion(void);
```

---

## Adding a New Service

> Before starting, read the [Design Philosophy](#design-philosophy) section. All services in this project follow the queue-only communication pattern — please do the same.

### Step 1 — Create your service files

`my_service.h` / `my_service.c` — own all state internally, expose only what's needed:

```c
// my_service.h
typedef enum {
    MY_EVENT_STARTED,
    MY_EVENT_STOPPED,
    MY_EVENT_STOP_REQUESTED,   // sent by supervisor to trigger clean shutdown
    MY_EVENT_ERROR,
} my_event_type_t;

typedef struct {
    my_event_type_t type;
    // ... event-specific data fields ...
} my_service_message_t;

void           my_service_start(void);
void           my_service_stop(void);     // sends STOP_REQUESTED via queue
QueueHandle_t  my_service_get_queue(void);
bool           my_service_is_healthy(void);
```

```c
// my_service.c — all state in one private struct, no shared variables
typedef struct {
    QueueHandle_t event_queue;
    TaskHandle_t  task_handle;
    bool          is_running;
} my_service_ctx_t;

static my_service_ctx_t s_ctx = {0};

void my_service_stop(void) {
    // Correct: signal via queue — task tears itself down
    my_service_message_t msg = { .type = MY_EVENT_STOP_REQUESTED };
    if (s_ctx.event_queue != NULL) {
        xQueueSend(s_ctx.event_queue, &msg, pdMS_TO_TICKS(100));
    }
}
```

### Step 2 — Add priority constants in `priorities.h`

```c
#define PRIO_MY_SUPERVISOR   15   // must be < PRIO_SUPERVISOR (24)
#define PRIO_MY_SERVICE      12   // must be < PRIO_MY_SUPERVISOR
```

### Step 3 — Write a supervisor wrapper in `system.c`

```c
void my_supervisor(void *arg) {
    static const char *TAG = "my-super";
    ESP_LOGI(TAG, "Starting");

    my_service_stop();    // tear down any leftover inner task first
    my_service_start();

    QueueHandle_t q = wait_for_queue(my_service_get_queue, 500);
    if (q == NULL) {
        ESP_LOGE(TAG, "Failed to get queue — exiting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Running");

    while (1) {
        my_service_message_t evt;
        if (xQueueReceive(q, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (evt.type) {
                case MY_EVENT_STARTED: ESP_LOGI(TAG, "Service started"); break;
                case MY_EVENT_ERROR:   ESP_LOGE(TAG, "Service error");   break;
                default: break;
            }
        }

        if (!my_service_is_healthy()) {
            ESP_LOGW(TAG, "Health check failed");
        }
    }
    vTaskDelete(NULL);
}
```

### Step 4 — Register in the services array in `system.c`

```c
const service_def_t services[] = {
    // ... existing entries ...
    {"my-service", my_supervisor, 4096, PRIO_MY_SUPERVISOR, RESTART_ALWAYS, false, NULL},
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL}  // sentinel — keep last
};
```

### Step 5 — Add your files to `main/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "main.c" "supervisor.c" "system.c" ... "my_service.c"
    ...
)
```

---

## Build & Configuration

### Prerequisites

- ESP-IDF v5.x with `IDF_PATH` set
- `espressif__onewire_bus` component (listed under `PRIV_REQUIRES`)
- Target set: `idf.py set-target esp32p4`

### CMakeLists.txt (main component)

```cmake
idf_component_register(
    SRCS
        "main.c"
        "supervisor.c"
        "system.c"
        "ethernet_service.c"
        "ethernet_setup.c"
        "mqtt_service.c"
        "app_mqtt.c"
        "ds18b20_temp.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer nvs_flash esp_eth esp_netif driver mqtt
    PRIV_REQUIRES
        espressif__onewire_bus
)
```

### Recommended sdkconfig Options

| Option | Value |
|--------|-------|
| `CONFIG_MQTT_BROKER_URI` | `mqtt://<your-broker-ip>` |
| `CONFIG_ESP_TASK_WDT` | `y` |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | `30` |
| `CONFIG_FREERTOS_UNICORE` | `n` |
| `CONFIG_FREERTOS_HZ` | `1000` |
| `CONFIG_LOG_DEFAULT_LEVEL` | `INFO` |

---

## Hardware Pin Assignment

| Signal | GPIO | Notes |
|--------|------|-------|
| ETH MDC | 31 | Management Data Clock |
| ETH MDIO | 52 | Management Data I/O |
| ETH PHY Reset | 51 | Active-low, driven by driver |
| ETH PHY Address | — | Strapped to 1 |
| DS18B20 1-Wire | 6 | Internal pull-up enabled via RMT config |

> For 1-Wire cable runs longer than ~20 cm, use an external **4.7 kΩ** pull-up resistor to 3.3 V.

---



### v1.0 (February 2025)

- Initial release

---

## License

MIT — see [LICENSE](LICENSE) for details.

---

*Tested on ESP32-P4 · ESP-IDF v5.x · FreeRTOS*
