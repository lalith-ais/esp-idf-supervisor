/*
 * ethernet_service.c  (v1.2 — hardened)
 *
 * HARDENING vs v1.1:
 *  [1] Queue-full logging  -- every xQueueSend(timeout=0) now warns on drop
 *  [2] volatile bool       -- is_connected / has_ip / is_running marked
 *                             volatile for safe dual-core reads
 *  [3] Heartbeat           -- supervisor_heartbeat() called each loop iter
 *                             so the supervisor can detect stuck tasks
 *  [4] Shutdown via queue  -- ETH_EVENT_STOP_REQUESTED replaces timed delay
 *                             + force-delete; task owns its own teardown
 */

#include "ethernet_service.h"
#include "ethernet_setup.h"
#include "supervisor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <string.h>

static const char *TAG = "eth-service";

/* [1] Helper -- log on queue full */
static inline void queue_send_warn(QueueHandle_t q,
                                   const eth_service_message_t *msg,
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
    QueueHandle_t  event_queue;
    volatile bool  is_running;       /* [2] */
    volatile bool  is_connected;     /* [2] */
    volatile bool  has_ip;           /* [2] */
    esp_eth_handle_t *eth_handles;
    uint8_t        eth_cnt;
    TaskHandle_t   task_handle;
} eth_service_ctx_t;

static eth_service_ctx_t eth_ctx = {0};

/* -------------------------------------------------------------------------
 * Callbacks from ethernet_setup (fired on ESP-IDF event loop task)
 * ------------------------------------------------------------------------- */

static void eth_ip_obtained_callback(void)
{
    if (eth_ctx.event_queue == NULL) return;

    eth_service_message_t msg = { .type = ETH_EVENT_GOT_IP };
    char ip_addr[16];
    ethernet_get_ip(ip_addr, sizeof(ip_addr));
    strncpy(msg.data.got_ip.ip, ip_addr, sizeof(msg.data.got_ip.ip) - 1);

    queue_send_warn(eth_ctx.event_queue, &msg, "GOT_IP");  /* [1] */
}

static void eth_disconnect_callback(void)
{
    if (eth_ctx.event_queue == NULL) return;

    eth_service_message_t msg = { .type = ETH_EVENT_DISCONNECTED };
    queue_send_warn(eth_ctx.event_queue, &msg, "DISCONNECTED");  /* [1] */
}

/* -------------------------------------------------------------------------
 * Main service task
 * ------------------------------------------------------------------------- */

static void ethernet_service_task(void *arg)
{
    ESP_LOGI(TAG, "Ethernet service starting");

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    /* Create queue BEFORE any callbacks can fire */
    eth_ctx.event_queue  = xQueueCreate(10, sizeof(eth_service_message_t));
    eth_ctx.is_running   = true;
    eth_ctx.task_handle  = xTaskGetCurrentTaskHandle();
    eth_ctx.is_connected = false;
    eth_ctx.has_ip       = false;

    if (eth_ctx.event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
#endif
        vTaskDelete(NULL);
        return;
    }

    ethernet_set_ip_callback(eth_ip_obtained_callback);
    ethernet_set_disconnect_callback(eth_disconnect_callback);

    esp_err_t ret = ethernet_init(&eth_ctx.eth_handles, &eth_ctx.eth_cnt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet hardware init failed");
        eth_service_message_t err_msg = {
            .type = ETH_EVENT_ERROR,
            .data.error.error = ret
        };
        queue_send_warn(eth_ctx.event_queue, &err_msg, "ERROR");  /* [1] */
        vQueueDelete(eth_ctx.event_queue);
        eth_ctx.event_queue = NULL;
#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_delete(NULL);
#endif
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Running -- waiting for events...");
    char last_ip[16] = {0};

    while (eth_ctx.is_running) {
        eth_service_message_t msg;

        if (xQueueReceive(eth_ctx.event_queue, &msg,
                          pdMS_TO_TICKS(1000)) == pdTRUE) {

            switch (msg.type) {
                case ETH_EVENT_GOT_IP:
                    ESP_LOGI(TAG, "Got IP: %s", msg.data.got_ip.ip);
                    eth_ctx.is_connected = true;
                    eth_ctx.has_ip       = true;
                    strncpy(last_ip, msg.data.got_ip.ip, sizeof(last_ip) - 1);
                    break;

                case ETH_EVENT_DISCONNECTED:
                    ESP_LOGI(TAG, "Disconnected");
                    eth_ctx.is_connected = false;
                    eth_ctx.has_ip       = false;
                    memset(last_ip, 0, sizeof(last_ip));
                    break;

                case ETH_EVENT_STARTED:
                    ESP_LOGI(TAG, "Ethernet started");
                    break;

                case ETH_EVENT_STOPPED:
                    ESP_LOGI(TAG, "Ethernet stopped");
                    eth_ctx.is_connected = false;
                    eth_ctx.has_ip       = false;
                    break;

                case ETH_EVENT_ERROR:
                    ESP_LOGE(TAG, "Hardware error: %s",
                             esp_err_to_name(msg.data.error.error));
                    eth_ctx.is_running = false;
                    break;

                /* [4] Clean shutdown requested via queue */
                case ETH_EVENT_STOP_REQUESTED:
                    ESP_LOGI(TAG, "Stop requested -- shutting down");
                    eth_ctx.is_running = false;
                    break;

                default:
                    break;
            }
        }

        /* Poll hardware state as a backup for missed callbacks */
        bool hw_connected = ethernet_is_connected();
        if (hw_connected != eth_ctx.is_connected) {
            eth_ctx.is_connected = hw_connected;
            eth_service_message_t status = {
                .type = hw_connected ? ETH_EVENT_CONNECTED : ETH_EVENT_DISCONNECTED
            };
            if (hw_connected) {
                uint8_t mac[6];
                ethernet_get_mac(mac);
                memcpy(status.data.connected.mac, mac, 6);
            } else {
                eth_ctx.has_ip = false;
                memset(last_ip, 0, sizeof(last_ip));
            }
            queue_send_warn(eth_ctx.event_queue, &status,  /* [1] */
                            hw_connected ? "CONNECTED" : "DISCONNECTED");
        }

        /* Poll for IP if connected but none recorded yet */
        if (eth_ctx.is_connected && !eth_ctx.has_ip) {
            char current_ip[16] = {0};
            ethernet_get_ip(current_ip, sizeof(current_ip));
            if (strlen(current_ip) > 0 && strcmp(current_ip, "0.0.0.0") != 0) {
                eth_ctx.has_ip = true;
                strncpy(last_ip, current_ip, sizeof(last_ip) - 1);
                eth_service_message_t ip_msg = { .type = ETH_EVENT_GOT_IP };
                strncpy(ip_msg.data.got_ip.ip, current_ip,
                        sizeof(ip_msg.data.got_ip.ip) - 1);
                queue_send_warn(eth_ctx.event_queue, &ip_msg, "GOT_IP_POLL"); /* [1] */
            }
        }

        /* [3] Pet heartbeat so supervisor knows we are not stuck */
        supervisor_heartbeat("ethernet");

#ifdef CONFIG_ESP_TASK_WDT
        esp_task_wdt_reset();
#endif
    }

    /* Cleanup */
    ESP_LOGI(TAG, "Cleaning up...");
    if (eth_ctx.eth_handles != NULL) {
        ethernet_deinit(eth_ctx.eth_handles, eth_ctx.eth_cnt);
        eth_ctx.eth_handles = NULL;
    }
    if (eth_ctx.event_queue != NULL) {
        vQueueDelete(eth_ctx.event_queue);
        eth_ctx.event_queue = NULL;
    }
    eth_ctx.task_handle = NULL;

#ifdef CONFIG_ESP_TASK_WDT
    esp_task_wdt_delete(NULL);
#endif
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void ethernet_service_start(void)
{
    if (eth_ctx.task_handle != NULL) {
        ESP_LOGW(TAG, "Already running");
        return;
    }

    xTaskCreate(ethernet_service_task, "eth-service",
                12288, NULL, PRIO_ETH_SERVICE,
                &eth_ctx.task_handle);
}

/* [4] Shutdown via queue message -- task owns its own teardown */
void ethernet_service_stop(void)
{
    if (eth_ctx.event_queue != NULL) {
        eth_service_message_t msg = { .type = ETH_EVENT_STOP_REQUESTED };
        if (xQueueSend(eth_ctx.event_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "Stop queue send failed -- task may already be gone");
        }
        /* Give the task time to process and self-delete */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    eth_ctx.task_handle = NULL;
}

QueueHandle_t ethernet_service_get_queue(void)  { return eth_ctx.event_queue;  }
bool ethernet_service_is_connected(void)         { return eth_ctx.is_connected; }
bool ethernet_service_has_ip(void)               { return eth_ctx.has_ip;       }

const char *ethernet_service_get_ip(void)
{
    static char ip[16] = {0};
    if (eth_ctx.has_ip) {
        ethernet_get_ip(ip, sizeof(ip));
    }
    return ip;
}
