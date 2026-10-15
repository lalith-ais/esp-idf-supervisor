# ESP32-P4 Supervisor / Control Node

> A FreeRTOS service supervision framework for ESP-IDF v5.x — start, monitor, and automatically restart application tasks, inspired by Unix `init` / `systemd`. On top of it runs a **MAC-addressed MQTT control node**: temperature telemetry, four MQTT-controlled relay outputs, and a small OLED status display.

Tested on real hardware: **ESP32-P4** with internal Ethernet (IP101 PHY), MQTT broker, DS18B20 temperature sensors and SSD1306 i2c OLED display.

---

## Table of Contents

- [Features](#features)
- [MQTT Topic Reference](#mqtt-topic-reference)
- [Relay Control](#relay-control)
- [Design Philosophy](#design-philosophy)
- [Architecture](#architecture)
- [File Structure](#file-structure)
- [Quick Start](#quick-start)
- [Task Priority Hierarchy](#task-priority-hierarchy)
- [Supervisor](#supervisor)
- [Services](#services)
  - [Network Service](#network-service)
  - [MQTT Service](#mqtt-service)
  - [DS18B20 Temperature Service](#ds18b20-temperature-service)
  - [Display Service](#display-service)
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
- **Heartbeat / stuck-task detection** — optional per-service `heartbeat_timeout_s`
- **Non-blocking exponential back-off** — failed restarts don't stall the supervisor loop
- **Essential service support** — triggers `esp_restart()` if a critical service can't recover; crash reason is persisted in NVS
- **Two-tier supervision model** — supervisor → wrapper task → inner service task
- **Transport-agnostic networking** — `network_service` sits above a transport vtable (Ethernet today, WiFi possible by swapping one pointer in `system.c`)
- **MAC-derived node identity** — one binary for every unit; client ID and all topics are built at runtime from the Ethernet MAC (e.g. `30EDA0EA52E4`)
- **MQTT** publish/subscribe, LWT (`online`/`offline`), periodic health JSON, automatic reconnection on network loss
- **4 relay outputs** switched by MQTT (`on` / `off`)
- **DS18B20** 1-Wire temperature sensing, published to MQTT every 30 s
- **SSD1306 OLED** showing a clock and a two-line text zone, driven over MQTT
- **Centralised priority definitions** — no magic numbers anywhere
- **ESP-IDF v5.x compatible**

---

## MQTT Topic Reference

All topics are `<root>/<MAC>/<name>`. `<root>` is `CONFIG_MQTT_TOPIC_ROOT` (default **empty**), so topics start with a single `/`. `<MAC>` is the 12-digit upper-case hex Ethernet MAC with no separators — for example `30EDA0EA52E4`. The MAC is printed at boot as `Node MAC ID: ...`.

| Topic | Direction | Payload | QoS / retain | Notes |
|-------|-----------|---------|--------------|-------|
| `/<MAC>/status` | node → broker | `online` / `offline` | 1 / retained | `online` on connect; `offline` is the LWT |
| `/<MAC>/health` | node → broker | JSON `{"uptime_s":…,"heap_free":…,"ip":"…"}` (plus `"crash"` if the supervisor recorded one) | 1 / no | every 30 s |
| `/<MAC>/temperature` | node → broker | e.g. `21.25` | 0 / no | used when **one** DS18B20 is found |
| `/<MAC>/temperature/N` | node → broker | e.g. `16.44` | 0 / no | used when **several** sensors are found; `N` = 0-based index in 1-Wire enumeration order |
| `/<MAC>/relay1` … `/<MAC>/relay4` | broker → node | `on` / `off` (case-insensitive) | 0 | see [Relay Control](#relay-control) |
| `/<MAC>/text` | broker → node | any text, max 63 chars | 0 | shown in the OLED text zone |
| `/<MAC>/cmd` | broker → node | `led_on` / `led_off` / `reboot` | 0 | **log-only stubs, no action** — see [Known Issues](#known-issues--limitations) |
| `/SYS/time` | broker → node | `HH:MM` | 0 | shared clock for the OLED; not per-unit |

The MQTT client ID is `<CONFIG_MQTT_CLIENT_ID_PREFIX><MAC>` (prefix defaults to empty).

---

## Relay Control

Four relay outputs are driven directly from the MQTT message callback (`mqtt_message_callback()` in `mqtt_service.c`) using `gpio_set_level()` — no queue hop.

| Topic | GPIO |
|-------|------|
| `/<MAC>/relay1` | 23 |
| `/<MAC>/relay2` | 26 |
| `/<MAC>/relay3` | 27 |
| `/<MAC>/relay4` | 32 |

Behaviour:

- At start-up all four GPIOs are configured as push-pull outputs and driven **LOW (off)** (`relay_gpio_init()`).
- The node subscribes to the four relay topics every time the broker connection is (re)established.
- Payload `on` sets the pin **HIGH**; payload `off` sets it **LOW**. Matching is case-insensitive (`ON`, `On`, `on`). Anything else logs a warning and changes nothing.
- The logic is **active-high**. If your relay board is active-low, on/off are inverted and all relays will be energised at boot — check before connecting a load.
- Relay state is **not stored** on the node and **not published back**. After a reboot every relay is off, unless the broker holds a *retained* command, which is applied as soon as the node re-subscribes.

Example (broker `192.168.124.4`, unit `30EDA0EA52E4`):

```bash
mosquitto_pub -h 192.168.124.4 -t /30EDA0EA52E4/relay1 -m on
mosquitto_pub -h 192.168.124.4 -t /30EDA0EA52E4/relay1 -m off

# watch everything the unit publishes
mosquitto_sub -h 192.168.124.4 -t '/30EDA0EA52E4/#' -v
```

Expected serial log on a command:

```
I mqtt-service: RX: /30EDA0EA52E4/relay1 -> on
I mqtt-service: Relay relay1 ON  (GPIO23)
```

---

## Design Philosophy

https://gist.github.com/lalith-ais/1036a2538996b3b46d788a4181b656c3

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
        ├─ network_supervisor            ← system.c   [priority 23]
        │    └─ network_service_task     ← network_service.c  [priority 22]
        │         └─ ethernet_transport  ← ethernet_transport.c (transport vtable)
        │              └─ ethernet_setup ← ethernet_setup.c     (HW driver)
        │
        ├─ mqtt_supervisor               ← system.c   [priority 20]
        │    ├─ mqtt_service_task        ← mqtt_service.c   [priority 19]
        │    │     └─ relay GPIO control (in mqtt_message_callback)
        │    └─ mqtt_publish_task        ← mqtt_service.c   [priority  5]  (health JSON)
        │         └─ app_mqtt            ← app_mqtt.c  (ESP-IDF MQTT wrapper)
        │
        ├─ ds18b20_temp_supervisor       ← system.c   [priority 10]
        │    └─ ds18b20_temp_task        ← ds18b20_temp.c   [priority  5]
        │          └─ 1-Wire RMT bus     (espressif__onewire_bus component)
        │
        └─ display_supervisor            ← system.c   [priority 10]
             └─ display_service_task     ← display_service.c [priority 4]
                   └─ SSD1306 over I2C
```

Data flow between services is by function call into thread-safe setters (`display_service_set_text()`, `mqtt_service_publish()`) and by queues for events.

---

## File Structure

```
control-node/
├── CMakeLists.txt
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild       # CONFIG_MQTT_BROKER_URI
    ├── idf_component.yml
    ├── main.c                  # Boot entry point
    ├── priorities.h            # All FreeRTOS priority constants
    ├── supervisor.h/.c         # Supervisor API + implementation (heartbeat, NVS crash record)
    ├── system.h/.c             # Supervisor task bodies + services[] registry
    ├── network_service.h/.c    # Transport-agnostic network state, MAC + IP accessors
    ├── network_transport.h     # Transport driver interface (vtable)
    ├── ethernet_transport.h/.c # Ethernet implementation of the transport vtable
    ├── ethernet_setup.h/.c     # Low-level ESP-IDF Ethernet driver (IP101 PHY)
    ├── mqtt_service.h/.c       # MQTT wrapper — MAC node ID, topics, LWT, health, relays
    ├── app_mqtt.h/.c           # Thin ESP-IDF MQTT client wrapper
    ├── ds18b20_temp.h/.c       # DS18B20 1-Wire temperature service
    ├── display_service.h/.c    # SSD1306 OLED service (clock + text zone)
    ├── display_font.h          # Roboto Condensed Bold bitmap font
    └── ethernet_service.h/.c   # LEGACY — superseded by network_service, not built
```

---

## Quick Start

### 1. Set up the IDF environment

```bash
cd control-node
idf.py set-target esp32p4
```

### 2. Configure

```bash
idf.py menuconfig
```

The broker address is under **Example Configuration → MQTT Broker Uri** (`CONFIG_MQTT_BROKER_URI`, default `mqtt://192.168.124.4`). No per-unit settings are needed: the same binary can be flashed to every unit because identity comes from the MAC.

Recommended: `CONFIG_ESP_TASK_WDT=y`, `CONFIG_FREERTOS_HZ=1000` (see [Build & Configuration](#build--configuration)).

### 3. Build and flash

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### 4. Check it works

1. In the boot log, find the node ID and subscriptions:
   ```
   Node MAC ID: 30EDA0EA52E4
   Subscribed: /30EDA0EA52E4/relay1 -> GPIO23
   ...
   Published: /30EDA0EA52E4/status = online (retained)
   ```
2. Watch the unit: `mosquitto_sub -h <broker> -t '/<MAC>/#' -v` — you should see `status`, `health` and `temperature` messages.
3. Switch a relay: `mosquitto_pub -h <broker> -t /<MAC>/relay1 -m on`.

---

## Task Priority Hierarchy

Defined in `priorities.h`. **Never use numeric literals** for task priorities in application code — always use these constants.

| Constant | Value | Task |
|----------|-------|------|
| `PRIO_SUPERVISOR` | 24 | supervisor (init) — must be highest |
| `PRIO_ETH_SUPERVISOR` | 23 | `network_supervisor` wrapper |
| `PRIO_ETH_SERVICE` | 22 | network service inner task |
| `PRIO_MQTT_SUPERVISOR` | 20 | `mqtt_supervisor` wrapper |
| `PRIO_MQTT_SERVICE` | 19 | `mqtt-service` inner task |
| `PRIO_DS18B20_SUPERVISOR` | 10 | `ds18b20_temp_supervisor` and `display_supervisor` wrappers |
| `PRIO_MQTT_PUBLISH` | 5 | `mqtt-publish` (health) inner task |
| `PRIO_DS18B20_SERVICE` | 5 | `ds18b20-temp` inner task |
| `PRIO_DISPLAY_SERVICE` | 4 | `display` inner task |

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

// Returns true if every essential service is alive and not stuck.
bool supervisor_is_healthy(void);

// Called by a service task to report liveness (see heartbeat_timeout_s).
void supervisor_heartbeat(const char *name);

// Name of the last essential service that forced a reboot (read from NVS), or NULL.
// Included as "crash" in the /<MAC>/health JSON when present.
const char *supervisor_get_last_crash(void);
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
    uint16_t          heartbeat_timeout_s; // 0 = disabled; else service must call
                                           // supervisor_heartbeat(name) within this time
} service_def_t;
```

#### Service registry (`system.c`)

```c
const service_def_t services[] = {
    /* name          entry                   stack  priority                 restart         essential ctx   hb_s */
    {"ethernet",     network_supervisor,     12288, PRIO_ETH_SUPERVISOR,     RESTART_ALWAYS, true,  NULL, 30},
    {"mqtt",         mqtt_supervisor,         8192, PRIO_MQTT_SUPERVISOR,    RESTART_ALWAYS, false, NULL, 30},
    {"ds18b20-temp", ds18b20_temp_supervisor, 4096, PRIO_DS18B20_SUPERVISOR, RESTART_ALWAYS, false, NULL, 60},
    {"display",      display_supervisor,      4096, PRIO_DS18B20_SUPERVISOR, RESTART_ALWAYS, false, NULL, 0},
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL, 0}  /* sentinel */
};
```

---

## Services

### Network Service

Files: `network_service.h/.c`, `network_transport.h`, `ethernet_transport.h/.c`, `ethernet_setup.h/.c`

`network_service.h` is the only networking header the rest of the application includes. It exposes no transport concepts, so Ethernet can be replaced by WiFi by changing the transport pointer passed to `network_service_start()` in `network_supervisor()` (`system.c`).

#### Event Types

```c
typedef enum {
    NET_EVENT_CONNECTED,        // link up — MAC address available
    NET_EVENT_DISCONNECTED,     // link lost
    NET_EVENT_GOT_IP,           // DHCP lease obtained
    NET_EVENT_STARTED,
    NET_EVENT_STOPPED,
    NET_EVENT_ERROR,            // unrecoverable hardware error
    NET_EVENT_STOP_REQUESTED    // sent by network_service_stop()
} net_event_type_t;
```

#### Public API

```c
void           network_service_start(const network_transport_t *transport);
void           network_service_stop(void);
QueueHandle_t  network_service_get_queue(void);   // Items: net_service_message_t
bool           network_service_is_connected(void);
bool           network_service_has_ip(void);
const char    *network_service_get_ip(void);
esp_err_t      network_service_get_mac(uint8_t mac[6]);   // used for node-ID derivation
```

---

### MQTT Service

Files: `mqtt_service.h/.c`, `app_mqtt.h/.c`

Start-up sequence in `mqtt_service_task()`:

1. `relay_gpio_init()` — relay pins 23/26/27/32 configured as outputs, all LOW.
2. Wait up to 10 s for a non-zero MAC from the network service.
3. Derive `s_mac_id` (12-char upper-hex) and build client ID, publish/subscribe topics and LWT topic.
4. Wait for an IP, connect to the broker (LWT `/<MAC>/status` = `offline`, QoS 1, retained).
5. On connect: subscribe to `/<MAC>/cmd`, `/SYS/time`, `/<MAC>/text`, `/<MAC>/relay1..4`; publish `online` (retained).
6. A health task publishes the JSON health payload every 30 s.

`s_mac_id` is a global defined in `mqtt_service.c` and referenced via `extern` by `ds18b20_temp.c`; it is only valid after step 3.

#### Configuration

| Key | Default | Where | Description |
|-----|---------|-------|-------------|
| `CONFIG_MQTT_BROKER_URI` | `mqtt://192.168.124.4` | `Kconfig.projbuild` (menuconfig) | Broker URI |
| `CONFIG_MQTT_CLIENT_ID_PREFIX` | `""` | compile-time fallback in `mqtt_service.c` | Prefix for client ID |
| `CONFIG_MQTT_TOPIC_ROOT` | `""` | compile-time fallback in `mqtt_service.c` | Prefix for all topics |
| `CONFIG_MQTT_PUBLISH_INTERVAL_MS` | `5000` | compile-time fallback | Publish interval |
| `CONFIG_MQTT_HEALTH_INTERVAL_MS` | `30000` | compile-time fallback | Health JSON interval |

Only the broker URI has a Kconfig entry. The others are `#ifndef` fallbacks, so changing them means adding Kconfig entries or passing `-D` flags. If you change `CONFIG_MQTT_TOPIC_ROOT`, also change `MQTT_TOPIC_ROOT` in `ds18b20_temp.c`, which is hard-coded to `""`.

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

Incoming messages are dispatched in `mqtt_message_callback()` (runs in the ESP-IDF MQTT client context): relay topics → GPIO, `/<MAC>/text` and `/SYS/time` → display service, and every message is also forwarded to the service queue as `MQTT_SERVICE_EVENT_MESSAGE_RECEIVED`. Topic buffers are 128 bytes and payload buffers 512 bytes in `app_mqtt.c`; the queue copy is truncated to 63/255 chars.

---

### DS18B20 Temperature Service

Files: `ds18b20_temp.h/.c`

Every 30 s the service triggers a conversion on all sensors, pushes a `ds18b20_reading_t` per sensor on its event queue, and publishes the value (`%.2f`, QoS 0, not retained) to MQTT.

#### Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `DS18B20_MAX_SENSORS` | `4` | Maximum sensors enumerated on the bus |
| `DS18B20_DEFAULT_GPIO` | `6` | GPIO pin for 1-Wire data line |
| `HEALTH_STALE_S` | `120` | Seconds without a reading before `is_healthy()` returns false |

#### Reading Queue

```c
typedef struct {
    int   sensor_index;   // 0-based sensor index
    float temperature;    // degrees Celsius
} ds18b20_reading_t;
```

#### MQTT Topics

| Topic | Condition |
|-------|-----------|
| `/<MAC>/temperature` | exactly one sensor on the bus |
| `/<MAC>/temperature/N` | more than one sensor — `N` is the zero-based enumeration index |

Index order is the 1-Wire search order (the sensor ROM addresses are logged at boot), not a fixed physical position. Record which index is which sensor when you install them.

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

### Display Service

Files: `display_service.h/.c`, `display_font.h`

128×64 SSD1306 on I2C (SDA GPIO 7, SCL GPIO 8, address `0x3C`, overridable via `CONFIG_OLED_SDA_GPIO` / `CONFIG_OLED_SCL_GPIO` / `CONFIG_OLED_I2C_ADDR`).

| Rows | Content |
|------|---------|
| 0–15 | Clock `HH:MM` (from `/SYS/time`) |
| 16–55 | Two centred text lines (from `/<MAC>/text`), word-wrapped; `\n` forces a break; empty payload clears |

On MQTT disconnect the clock shows `----` and the text is cleared. Text is truncated at 63 characters.

```c
void display_service_start(void);
void display_service_stop(void);
bool display_service_is_running(void);
void display_service_set_time(const char *time_str);   // "HH:MM"
void display_service_set_text(const char *text);       // thread-safe, non-blocking
void display_service_set_mqtt_connected(bool connected);
```

> The header comment in `display_service.h` still mentions the old `/controller/text` topic. The code actually subscribes to `/<MAC>/text`.

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
    {"my-service", my_supervisor, 4096, PRIO_MY_SUPERVISOR, RESTART_ALWAYS, false, NULL, 0},
    {NULL, NULL, 0, 0, RESTART_NEVER, false, NULL, 0}  // sentinel — keep last
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

- ESP-IDF v5.x (tested with v5.5.2) with `IDF_PATH` set
- Components pulled by the IDF component manager: `espressif/ds18b20` (`main/idf_component.yml`) and `espressif__onewire_bus`
- Target set: `idf.py set-target esp32p4`

### CMakeLists.txt (main component)

```cmake
idf_component_register(
    SRCS
        "main.c"
        "supervisor.c"
        "system.c"
        "network_service.c"
        "ethernet_transport.c"
        "ethernet_setup.c"
        "mqtt_service.c"
        "app_mqtt.c"
        "ds18b20_temp.c"
        "display_service.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer nvs_flash esp_eth esp_netif driver mqtt esp_lcd
    PRIV_REQUIRES espressif__onewire_bus
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
| OLED SDA | 7 | I2C, address 0x3C |
| OLED SCL | 8 | I2C |
| Relay 1 | 23 | `/<MAC>/relay1`, active-high |
| Relay 2 | 26 | `/<MAC>/relay2`, active-high |
| Relay 3 | 27 | `/<MAC>/relay3`, active-high |
| Relay 4 | 32 | `/<MAC>/relay4`, active-high |
| Console UART | 37 / 38 | ESP32-P4 default console pins (from boot log) |

> For 1-Wire cable runs longer than ~20 cm, use an external **4.7 kΩ** pull-up resistor to 3.3 V.
>
> Relay outputs are 3.3 V logic straight from the GPIO. Drive real relays through a transistor/driver stage or an opto-isolated relay board; never switch a coil directly from the pin.

---

## Known Issues & Limitations

- **`/<MAC>/cmd` is a stub.** `led_on`, `led_off` and `reboot` only log a message; no GPIO is driven and `esp_restart()` is not called.
- **No relay state feedback.** Relay state is not published and not persisted; a node reboot turns all relays off (a retained command on the broker will re-apply on reconnect).
- **Active-high relays only**, and strict payload match (`on`/`off`, case-insensitive; trailing whitespace or newline is rejected).
- **Topic root is not fully configurable.** `MQTT_TOPIC_ROOT` in `ds18b20_temp.c` is hard-coded to `""` and must be kept in step with `CONFIG_MQTT_TOPIC_ROOT`; only the broker URI has a Kconfig entry.
- **Single shared clock.** `/SYS/time` is not per-unit.
- **Stale comments.** Header comments in `ds18b20_temp.c` mention `/ESP32P4/<MAC>/temperature` and `display_service.h` mentions `/controller/text`; the live topics are `/<MAC>/temperature` and `/<MAC>/text`.
- **Legacy files.** `ethernet_service.h/.c` are superseded by `network_service` and are not in the build.
- **No authentication/TLS.** The client connects to `mqtt://` on a trusted LAN; anyone who can reach the broker can switch the relays.

---

## Changelog

### Current
- Transport-agnostic `network_service` replaces `ethernet_service`
- MAC-derived node ID, client ID and topics (`/<MAC>/…`)
- LWT status topic and 30 s health JSON
- Four MQTT-controlled relays (GPIO 23, 26, 27, 32)
- DS18B20 temperatures published per unit
- SSD1306 display service (clock + text zone)
- Supervisor heartbeat checks and NVS crash record

### v1.0 (February 2025)

- Initial release

---

## License

MIT — see [LICENSE](LICENSE) for details.

---

*Tested on ESP32-P4 · ESP-IDF v5.x · FreeRTOS*
