/*
 * network_service.c - Transport-agnostic network state service
 *
 * Structural port of ethernet_service.c (v1.2 hardened).
 * ALL hardening from v1.2 is preserved:
 *
 *  [1] Queue-full logging  -- every xQueueSend(timeout=0) warns on drop
 *  [2] volatile bool       -- is_connected / has_ip / is_running
 *  [3] Heartbeat           -- supervisor_heartbeat() called each loop iter
 *  [4] Shutdown via queue  -- NET_EVENT_STOP_REQUESTED for clean teardown
 *
 * What changed vs ethernet_service.c:
 *  - All eth_* identifiers renamed net_* / network_*
 *  - ETH_EVENT_* enum replaced by NET_EVENT_* (network_service.h)
 *  - ethernet_init / ethernet_deinit / ethernet_is_connected / ethernet_get_ip
 *    replaced by transport vtable calls (transport->init, etc.)
 *  - ethernet_set_ip_callback / ethernet_set_disconnect_callback replaced by
 *    callbacks passed directly into transport->init()
 *  - supervisor_heartbeat tag changed from "ethernet" to transport->name
 *    so the supervisor registry entry name still matches
 */

#include "network_service.h"
#include "network_transport.h"
#include "supervisor.h"
#include "priorities.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <string.h>

static const char *TAG = "net-service";

/* -------------------------------------------------------------------------
 * [1] Queue-send helper -- logs on drop
 * ------------------------------------------------------------------------- */
static inline void queue_send_warn(QueueHandle_t q,
                                   const net_service_message_t *msg,
                                   const char *event_name)
{
    if (xQueueSend(q, msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full -- dropped event: %s", event_name);
    }
}

/* -------------------------------------------------------------------------
 * Service context
 * ------------------------------------------------------------------------- */
typedef struct {
    QueueHandle_t             event_queue;
    volatile bool             is_running;       /* [2] */
    volatile bool             is_connected;     /* [2] */
    volatile bool             has_ip;           /* [2] */
    const network_transport_t *transport;
    TaskHandle_t              task_handle;
    char                      ip[16];           /* stored on NET_EVENT_GOT_IP */
} net_service_ctx_t;

static net_service_ctx_t s_ctx = {0};

/* -------------------------------------------------------------------------
 * Transport callbacks -- fired from the transport's event handler task.
 * Must only post to the queue (ISR/event-task safe).
 * ------------------------------------------------------------------------- */
static void on_ip_acquired(const char *ip_str)
{
    if (s_ctx.event_queue == NULL) return;

    /* Set state immediately in the callback, before posting to the queue.
     * The supervisor drains the same public queue as the service task, so
     * it may consume NET_EVENT_GOT_IP before the task sees it.
     * State must not depend on the message being processed by the task. */
    strncpy(s_ctx.ip, ip_str, sizeof(s_ctx.ip) - 1);
    s_ctx.is_connected = true;
    s_ctx.has_ip       = true;

    net_service_message_t msg = { .type = NET_EVENT_GOT_IP };
    strncpy(msg.data.got_ip.ip, ip_str, sizeof(msg.data.got_ip.ip) - 1);
    queue_send_warn(s_ctx.event_queue, &msg, "GOT_IP");  /* [1] */
}

static void on_disconnected(void)
{
    if (s_ctx.event_queue == NULL) return;

    /* Clear state immediately -- same reason, don't wait for queue processing */
    s_ctx.is_connected = false;
    s_ctx.has_ip       = false;
    memset(s_ctx.ip, 0, sizeof(s_ctx.ip));

    net_service_message_t msg = { .type = NET_EVENT_DISCONNECTED };
    queue_send_warn(s_ctx.event_queue, &msg, "DISCONNECTED");  /* [1] */
}

/* -------------------------------------------------------------------------
 * Main service task
 * ------------------------------------------------------------------------- */
static void network_service_task(void *arg)
{
    const network_transport_t *transport = (const network_transport_t *)arg;

    ESP_LOGI(TAG, "Network service starting (transport: %s)", transport->name);

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    /* Create queue BEFORE transport init so callbacks can post safely */
    s_ctx.event_queue  = xQueueCreate(10, sizeof(net_service_message_t));
    s_ctx.is_running   = true;
    s_ctx.task_handle  = xTaskGetCurrentTaskHandle();
    s_ctx.is_connected = false;
    s_ctx.has_ip       = false;
    s_ctx.transport    = transport;

    if (s_ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
#endif
        vTaskDelete(NULL);
        return;
    }

    /* Initialise transport, hand it our two callbacks */
    esp_err_t ret = transport->init(on_ip_acquired, on_disconnected);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Transport init failed: %s", esp_err_to_name(ret));
        net_service_message_t err = {
            .type = NET_EVENT_ERROR,
            .data.error.error = ret
        };
        queue_send_warn(s_ctx.event_queue, &err, "ERROR");  /* [1] */
        vQueueDelete(s_ctx.event_queue);
        s_ctx.event_queue = NULL;
#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
#endif
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Running -- waiting for events...");

    while (s_ctx.is_running) {
        net_service_message_t msg;

        if (xQueueReceive(s_ctx.event_queue, &msg,
                          pdMS_TO_TICKS(1000)) == pdTRUE) {

            switch (msg.type) {
                case NET_EVENT_GOT_IP:
                    ESP_LOGI(TAG, "Got IP: %s", msg.data.got_ip.ip);
                    s_ctx.is_connected = true;
                    s_ctx.has_ip       = true;
                    strncpy(s_ctx.ip, msg.data.got_ip.ip, sizeof(s_ctx.ip) - 1);
                    break;

                case NET_EVENT_DISCONNECTED:
                    ESP_LOGI(TAG, "Disconnected");
                    s_ctx.is_connected = false;
                    s_ctx.has_ip       = false;
                    memset(s_ctx.ip, 0, sizeof(s_ctx.ip));
                    break;

                case NET_EVENT_STARTED:
                    ESP_LOGI(TAG, "Transport started");
                    break;

                case NET_EVENT_STOPPED:
                    ESP_LOGI(TAG, "Transport stopped");
                    s_ctx.is_connected = false;
                    s_ctx.has_ip       = false;
                    break;

                case NET_EVENT_ERROR:
                    ESP_LOGE(TAG, "Transport error: %s",
                             esp_err_to_name(msg.data.error.error));
                    s_ctx.is_running = false;
                    break;

                case NET_EVENT_STOP_REQUESTED:  /* [4] */
                    ESP_LOGI(TAG, "Stop requested -- shutting down");
                    s_ctx.is_running = false;
                    break;

                default:
                    break;
            }
        }

        /* Poll hardware link state as backup for missed callbacks */
        bool hw_connected = transport->is_connected();
        if (hw_connected != s_ctx.is_connected) {
            s_ctx.is_connected = hw_connected;
            net_service_message_t status = {
                .type = hw_connected ? NET_EVENT_CONNECTED
                                     : NET_EVENT_DISCONNECTED
            };
            if (hw_connected) {
                transport->get_mac(status.data.connected.mac);
            } else {
                s_ctx.has_ip = false;
            }
            queue_send_warn(s_ctx.event_queue, &status,  /* [1] */
                            hw_connected ? "CONNECTED" : "DISCONNECTED");
        }

        /* [3] Heartbeat -- use transport name so supervisor slot matches */
        supervisor_heartbeat(transport->name);

#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
#endif
    }

    /* Cleanup */
    ESP_LOGI(TAG, "Cleaning up...");
    transport->deinit();

    if (s_ctx.event_queue != NULL) {
        vQueueDelete(s_ctx.event_queue);
        s_ctx.event_queue = NULL;
    }
    s_ctx.task_handle  = NULL;
    s_ctx.is_connected = false;
    s_ctx.has_ip       = false;

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
#endif
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void network_service_start(const network_transport_t *transport)
{
    if (s_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "Already running");
        return;
    }
    if (transport == NULL) {
        ESP_LOGE(TAG, "transport vtable is NULL");
        return;
    }

    xTaskCreate(network_service_task, "net-service",
                12288, (void *)transport, PRIO_ETH_SERVICE,
                &s_ctx.task_handle);
}

/* [4] Shutdown via queue -- task owns its own teardown */
void network_service_stop(void)
{
    if (s_ctx.event_queue != NULL) {
        net_service_message_t msg = { .type = NET_EVENT_STOP_REQUESTED };
        if (xQueueSend(s_ctx.event_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "Stop queue send failed -- task may already be gone");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s_ctx.task_handle = NULL;
}

QueueHandle_t network_service_get_queue(void)    { return s_ctx.event_queue;  }
bool          network_service_is_connected(void) { return s_ctx.is_connected; }
bool          network_service_has_ip(void)        { return s_ctx.has_ip;       }

const char *network_service_get_ip(void)
{
    return s_ctx.ip;   /* populated on NET_EVENT_GOT_IP, cleared on disconnect */
}

esp_err_t network_service_get_mac(uint8_t mac[6])
{
    if (s_ctx.transport == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_ctx.transport->get_mac(mac);
}
