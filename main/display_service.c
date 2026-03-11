/*
 * display_service.c  (v3.0 -- two-zone layout, Roboto Condensed Bold)
 *
 * ZONES
 *   Yellow  rows  0-15  (pages 0-1):  HH:MM clock
 *   Blue    rows 16-35  (pages 2-3):  text line 1
 *   Blue    rows 36-55  (pages 4-5):  text line 2
 *   Blue    rows 56-63  (pages 6-7):  margin / unused
 *
 * FONT
 *   Roboto Condensed Bold, pre-rendered bitmaps in display_font.h.
 *   Variable-width glyphs -- each glyph stores its own advance width.
 *   Rendering: blit column-by-column into the target page range.
 *   No LVGL, no heap allocations after init.
 *
 * FRAMEBUFFER
 *   1024 bytes (128 cols x 8 pages x 1 bit/pixel).
 *   SSD1306 native page format: byte[page*128+col], bit0=top of page.
 *   Each zone occupies a contiguous page range -- zone writes never
 *   touch pages outside their range.
 *
 * MQTT TOPICS
 *   /SYS/time          -> display_service_set_time("HH:MM")
 *   /controller/text   -> display_service_set_text("any string")
 */

#include "display_service.h"
#include "display_font.h"
#include "priorities.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

static const char *TAG = "display";

/* ── hardware config ───────────────────────────────────────────────────── */
#ifndef CONFIG_OLED_SDA_GPIO
#define CONFIG_OLED_SDA_GPIO    7
#endif
#ifndef CONFIG_OLED_SCL_GPIO
#define CONFIG_OLED_SCL_GPIO    8
#endif
#ifndef CONFIG_OLED_I2C_ADDR
#define CONFIG_OLED_I2C_ADDR    0x3C
#endif

#define OLED_W      128
#define OLED_H       64
#define OLED_PAGES  (OLED_H / 8)           /* 8  */
#define FB_SIZE     (OLED_W * OLED_PAGES)  /* 1024 bytes */

/* ── zone definitions ──────────────────────────────────────────────────── */
/* Yellow zone: rows 0-15 = pages 0-1 */
#define ZONE_CLOCK_PAGE_START   0
#define ZONE_CLOCK_PAGE_COUNT   (FONT_CLOCK_CELL_H / 8)   /* 2 */

/* Blue text line 1: rows 16-35 = pages 2-3 */
#define ZONE_LINE1_PAGE_START   2
#define ZONE_LINE1_PAGE_COUNT   (FONT_TEXT_CELL_H / 8)    /* 2 ... wait, 20/8 isn't integer */

/* NOTE: FONT_TEXT_CELL_H=20px spans 2.5 pages. We use pages 2-3 (16px) for
 * line1 and pages 4-5 (16px) for line2, with the glyph vertically centred
 * in the 20px budget. Since pages are 8px each we allocate 3 pages (24px)
 * per text line and centre the 17px-tall glyphs within that space.
 * This gives a natural 4px gap between lines.                            */
#undef  ZONE_LINE1_PAGE_START
#undef  ZONE_LINE1_PAGE_COUNT

#define ZONE_LINE1_ROW_TOP    16   /* first pixel row of text line 1 */
#define ZONE_LINE2_ROW_TOP    38   /* first pixel row of text line 2  (+22px gap) */
#define TEXT_RENDER_Y          2   /* pixels from zone top to glyph top (centering) */

/* ── queue messages ────────────────────────────────────────────────────── */
#define MAX_TEXT_LEN  64

typedef enum {
    DISPLAY_MSG_TIME,
    DISPLAY_MSG_TEXT,
    DISPLAY_MSG_MQTT_STATE,
    DISPLAY_MSG_STOP,
} display_msg_type_t;

typedef struct {
    display_msg_type_t type;
    union {
        char time_str[6];             /* "HH:MM\0" */
        char text[MAX_TEXT_LEN];      /* arbitrary string from /controller/text */
        bool connected;
    } data;
} display_msg_t;

/* ── service context ───────────────────────────────────────────────────── */
typedef struct {
    QueueHandle_t  queue;
    TaskHandle_t   task_handle;
    volatile bool  is_running;
} display_ctx_t;

static display_ctx_t s_ctx = {0};

/* ── framebuffer ───────────────────────────────────────────────────────── */
static uint8_t s_fb[FB_SIZE];

/* ══════════════════════════════════════════════════════════════════════════
 * Framebuffer zone primitives
 * ══════════════════════════════════════════════════════════════════════════ */

/** Clear a horizontal band of pages (inclusive). */
static void fb_clear_rows(int row_start, int row_end)
{
    int page_start = row_start / 8;
    int page_end   = row_end   / 8;
    for (int page = page_start; page <= page_end; page++) {
        memset(&s_fb[page * OLED_W], 0x00, OLED_W);
    }
}

/**
 * Blit a variable-width glyph into the framebuffer.
 *
 * The glyph's page-format data covers cell_pages pages.
 * We write it starting at framebuffer page `fb_page_start`, column `x`.
 * Returns the advance width so the caller can step x forward.
 *
 * @param x              column to blit at (0..127)
 * @param fb_page_start  which framebuffer page the glyph's page-0 maps to
 * @param gi             glyph info (width + data pointer)
 * @param cell_pages     number of pages in the glyph data
 */
static void fb_blit_glyph(int x, int fb_page_start,
                            const glyph_info_t *gi, int cell_pages)
{
    if (x < 0 || x + gi->width > OLED_W) return;
    for (int p = 0; p < cell_pages; p++) {
        int fb_idx    = (fb_page_start + p) * OLED_W + x;
        int glyph_idx = p * gi->width;
        memcpy(&s_fb[fb_idx], &gi->data[glyph_idx], gi->width);
    }
}

/**
 * Measure the pixel width of a string using the text font.
 * Returns total advance width.
 */
static int text_measure(const char *str)
{
    int w = 0;
    for (; *str; str++) {
        uint8_t c = (uint8_t)*str;
        if (c < FONT_TEXT_FIRST || c > FONT_TEXT_LAST) continue;
        w += FONT_TEXT[c - FONT_TEXT_FIRST].width;
    }
    return w;
}

/**
 * Blit a string into the framebuffer, centred horizontally within OLED_W.
 * @param str           null-terminated string
 * @param row_top       pixel row of top of text zone (16 or 38)
 * @param render_y_off  pixel offset within zone for vertical centering
 */
static void fb_draw_text_line(const char *str, int row_top, int render_y_off)
{
    int total_w = text_measure(str);
    int x = (OLED_W - total_w) / 2;
    if (x < 0) x = 0;

    /* Which framebuffer page does row_top map to? */
    int fb_page_start = (row_top + render_y_off) / 8;
    /* We use FONT_TEXT_PAGES pages per glyph */

    for (const char *p = str; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c < FONT_TEXT_FIRST || c > FONT_TEXT_LAST) {
            x += 4;   /* unknown char: advance by space width */
            continue;
        }
        const glyph_info_t *gi = &FONT_TEXT[c - FONT_TEXT_FIRST];
        fb_blit_glyph(x, fb_page_start, gi, FONT_TEXT_PAGES);
        x += gi->width;
        if (x >= OLED_W) break;
    }
}

/**
 * Word-wrap text into at most 2 lines, each fitting within OLED_W pixels.
 * Supports explicit '\n' as a forced line break.
 * Fills line1 and line2 (both must be MAX_TEXT_LEN bytes).
 */
static void word_wrap(const char *text, char *line1, char *line2)
{
    line1[0] = '\0';
    line2[0] = '\0';

    if (!text || text[0] == '\0') return;

    /* Check for explicit newline first */
    const char *nl = strchr(text, '\n');
    if (nl) {
        int len1 = (int)(nl - text);
        if (len1 >= MAX_TEXT_LEN) len1 = MAX_TEXT_LEN - 1;
        strncpy(line1, text, len1);
        line1[len1] = '\0';
        strncpy(line2, nl + 1, MAX_TEXT_LEN - 1);
        /* Trim any trailing \n from line2 */
        char *end = line2 + strlen(line2) - 1;
        while (end >= line2 && *end == '\n') *end-- = '\0';
        return;
    }

    /* Automatic word-wrap */
    char buf[MAX_TEXT_LEN];
    strncpy(buf, text, MAX_TEXT_LEN - 1);
    buf[MAX_TEXT_LEN - 1] = '\0';

    char *word = strtok(buf, " ");
    char current[MAX_TEXT_LEN] = "";
    int  line_num = 1;

    while (word) {
        /* test needs to hold current + " " + word -- use a larger scratch
         * buffer so the compiler's format-truncation check is satisfied.  */
        char test[MAX_TEXT_LEN * 2];
        if (current[0] == '\0') {
            strncpy(test, word, MAX_TEXT_LEN - 1);
            test[MAX_TEXT_LEN - 1] = '\0';
        } else {
            snprintf(test, sizeof(test), "%s %s", current, word);
        }

        if (text_measure(test) <= OLED_W - 4) {   /* 2px margin each side */
            strncpy(current, test, MAX_TEXT_LEN - 1);
        } else {
            if (line_num == 1) {
                strncpy(line1, current, MAX_TEXT_LEN - 1);
                strncpy(current, word,  MAX_TEXT_LEN - 1);
                line_num = 2;
            } else {
                /* Line 2 full -- truncate remaining words */
                break;
            }
        }
        word = strtok(NULL, " ");
    }

    /* Flush remaining */
    if (line_num == 1) {
        strncpy(line1, current, MAX_TEXT_LEN - 1);
    } else {
        strncpy(line2, current, MAX_TEXT_LEN - 1);
    }
}

/* ── clock rendering ───────────────────────────────────────────────────── */

/**
 * Measure pixel width of a clock string using FONT_CLOCK.
 * Valid chars: '0'-'9' and ':'.
 */
static int clock_measure(const char *str)
{
    int w = 0;
    for (; *str; str++) {
        int idx = -1;
        if (*str >= '0' && *str <= '9') idx = *str - '0';
        else if (*str == ':')            idx = 10;
        if (idx >= 0) w += FONT_CLOCK[idx].width;
    }
    return w;
}

static void fb_draw_clock(const char *time_str)
{
    /* Clear yellow zone (pages 0-1) */
    fb_clear_rows(0, 15);

    int total_w = clock_measure(time_str);
    int x = (OLED_W - total_w) / 2;

    /* Clock glyphs are FONT_CLOCK_CELL_H=16px tall = 2 pages, starts at page 0 */
    for (const char *p = time_str; *p; p++) {
        int idx = -1;
        if (*p >= '0' && *p <= '9') idx = *p - '0';
        else if (*p == ':')          idx = 10;
        if (idx < 0) continue;
        const glyph_info_t *gi = &FONT_CLOCK[idx];
        fb_blit_glyph(x, 0, gi, FONT_CLOCK_PAGES);
        x += gi->width;
    }
}

static void fb_draw_clock_dashes(void)
{
    fb_clear_rows(0, 15);
    /* Draw "----" using the minus/hyphen glyph from the clock-sized text font.
     * We approximate with 4 dashes from the text font scaled to the clock zone. */
    const char *dashes = "- - - -";
    int total_w = text_measure(dashes);
    int x = (OLED_W - total_w) / 2;
    /* Render into page 0 (rows 0-7) -- single page, centred vertically in 16px */
    for (const char *p = dashes; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c < FONT_TEXT_FIRST || c > FONT_TEXT_LAST) continue;
        const glyph_info_t *gi = &FONT_TEXT[c - FONT_TEXT_FIRST];
        /* Blit only page 0 of the text glyph into framebuffer page 0 */
        if (x >= 0 && x + gi->width <= OLED_W) {
            memcpy(&s_fb[0 * OLED_W + x], &gi->data[0], gi->width);
        }
        x += gi->width;
    }
}

static void fb_draw_text_zone(const char *line1, const char *line2)
{
    /* Clear blue text zone (rows 16-63 = pages 2-7) */
    fb_clear_rows(16, 63);

    if (line1 && line1[0]) {
        fb_draw_text_line(line1, ZONE_LINE1_ROW_TOP, TEXT_RENDER_Y);
    }
    if (line2 && line2[0]) {
        fb_draw_text_line(line2, ZONE_LINE2_ROW_TOP, TEXT_RENDER_Y);
    }
}

/* ── full-white test pattern (keep for future panel diagnostics) ────────── */
static void __attribute__((unused)) fb_render_full_white(void)
{
    memset(s_fb, 0xFF, FB_SIZE);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Hardware init / cleanup  (matches ESP-IDF i2c_oled example)
 * ══════════════════════════════════════════════════════════════════════════ */

static esp_err_t hw_init(i2c_master_bus_handle_t   *out_bus,
                          esp_lcd_panel_io_handle_t *out_io,
                          esp_lcd_panel_handle_t    *out_panel)
{
    ESP_LOGI(TAG, "Initialize I2C bus");
    i2c_master_bus_config_t bus_config = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = CONFIG_OLED_SDA_GPIO,
        .scl_io_num                   = CONFIG_OLED_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, out_bus),
                        TAG, "i2c_new_master_bus failed");

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr            = CONFIG_OLED_I2C_ADDR,
        .scl_speed_hz        = 400 * 1000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .dc_bit_offset       = 6,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(*out_bus, &io_config, out_io),
                        TAG, "esp_lcd_new_panel_io_i2c failed");

    ESP_LOGI(TAG, "Install SSD1306 panel driver");
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = OLED_H,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config  = &ssd1306_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(*out_io, &panel_config, out_panel),
                        TAG, "esp_lcd_new_panel_ssd1306 failed");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(*out_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*out_panel));
    /* Panel mounted inverted -- mirror both axes to correct orientation */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(*out_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(*out_panel, true));

    ESP_LOGI(TAG, "SSD1306 ready  SDA=GPIO%d  SCL=GPIO%d  addr=0x%02X",
             CONFIG_OLED_SDA_GPIO, CONFIG_OLED_SCL_GPIO, CONFIG_OLED_I2C_ADDR);
    return ESP_OK;
}

static void hw_cleanup(i2c_master_bus_handle_t   bus,
                        esp_lcd_panel_io_handle_t io,
                        esp_lcd_panel_handle_t    panel)
{
    if (panel) { esp_lcd_panel_disp_on_off(panel, false); esp_lcd_panel_del(panel); }
    if (io)    esp_lcd_panel_io_del(io);
    if (bus)   i2c_del_master_bus(bus);
}

static inline void fb_flush(esp_lcd_panel_handle_t panel)
{
    esp_lcd_panel_draw_bitmap(panel, 0, 0, OLED_W, OLED_H, s_fb);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Display task
 * ══════════════════════════════════════════════════════════════════════════ */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Task starting");

    i2c_master_bus_handle_t   bus   = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;
    esp_lcd_panel_handle_t    panel = NULL;

    if (hw_init(&bus, &io, &panel) != ESP_OK) {
        ESP_LOGE(TAG, "HW init failed -- exiting");
        s_ctx.is_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Initial state: dashes in clock zone, empty text zone */
    memset(s_fb, 0, FB_SIZE);
    fb_draw_clock_dashes();
    fb_flush(panel);

    /* Persistent state across queue messages */
    char cur_time[6]          = "----";
    char cur_line1[MAX_TEXT_LEN] = "";
    char cur_line2[MAX_TEXT_LEN] = "";
    bool mqtt_connected        = false;

    display_msg_t msg;
    while (s_ctx.is_running) {
        if (xQueueReceive(s_ctx.queue, &msg, pdMS_TO_TICKS(10000)) != pdTRUE) {
            continue;
        }

        bool redraw = false;

        switch (msg.type) {

        case DISPLAY_MSG_TIME: {
            const char *ts = msg.data.time_str;
            if (strlen(ts) != 5 || ts[2] != ':'
                || !isdigit((unsigned char)ts[0])
                || !isdigit((unsigned char)ts[1])
                || !isdigit((unsigned char)ts[3])
                || !isdigit((unsigned char)ts[4])) {
                ESP_LOGW(TAG, "Bad time string: '%s'", ts);
                break;
            }
            int hh = (ts[0]-'0')*10 + (ts[1]-'0');
            int mm = (ts[3]-'0')*10 + (ts[4]-'0');
            if (hh > 23 || mm > 59) { ESP_LOGW(TAG, "Out-of-range: %s", ts); break; }
            strncpy(cur_time, ts, sizeof(cur_time) - 1);
            mqtt_connected = true;
            redraw = true;
            ESP_LOGI(TAG, "Time: %s", ts);
            break;
        }

        case DISPLAY_MSG_TEXT: {
            word_wrap(msg.data.text, cur_line1, cur_line2);
            redraw = true;
            ESP_LOGI(TAG, "Text: '%s' / '%s'", cur_line1, cur_line2);
            break;
        }

        case DISPLAY_MSG_MQTT_STATE:
            mqtt_connected = msg.data.connected;
            if (!mqtt_connected) {
                /* Clear everything on disconnect */
                strncpy(cur_time, "----", sizeof(cur_time) - 1);
                cur_line1[0] = '\0';
                cur_line2[0] = '\0';
                ESP_LOGI(TAG, "MQTT offline -- clearing display");
            }
            redraw = true;
            break;

        case DISPLAY_MSG_STOP:
            s_ctx.is_running = false;
            break;
        }

        if (redraw && s_ctx.is_running) {
            memset(s_fb, 0, FB_SIZE);
            if (mqtt_connected) {
                fb_draw_clock(cur_time);
            } else {
                fb_draw_clock_dashes();
            }
            fb_draw_text_zone(cur_line1, cur_line2);
            fb_flush(panel);
        }
    }

    /* Blank on clean shutdown */
    memset(s_fb, 0, FB_SIZE);
    fb_flush(panel);
    hw_cleanup(bus, io, panel);
    ESP_LOGI(TAG, "Task stopped");
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

void display_service_start(void)
{
    if (s_ctx.is_running) { ESP_LOGW(TAG, "Already running"); return; }
    s_ctx.queue = xQueueCreate(8, sizeof(display_msg_t));
    if (!s_ctx.queue) { ESP_LOGE(TAG, "Queue alloc failed"); return; }
    s_ctx.is_running = true;
    if (xTaskCreate(display_task, "display", 4096, NULL,
                    PRIO_DISPLAY_SERVICE, &s_ctx.task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        s_ctx.is_running = false;
        vQueueDelete(s_ctx.queue);
        s_ctx.queue = NULL;
    }
}

void display_service_stop(void)
{
    if (!s_ctx.is_running || !s_ctx.queue) return;
    display_msg_t msg = { .type = DISPLAY_MSG_STOP };
    xQueueSend(s_ctx.queue, &msg, pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(500));
    s_ctx.task_handle = NULL;
}

bool display_service_is_running(void) { return s_ctx.is_running; }

void display_service_set_time(const char *time_str)
{
    if (!s_ctx.is_running || !s_ctx.queue || !time_str) return;
    display_msg_t msg = { .type = DISPLAY_MSG_TIME };
    strncpy(msg.data.time_str, time_str, sizeof(msg.data.time_str) - 1);
    if (xQueueSend(s_ctx.queue, &msg, 0) != pdTRUE)
        ESP_LOGW(TAG, "Queue full -- time dropped");
}

void display_service_set_text(const char *text)
{
    if (!s_ctx.is_running || !s_ctx.queue) return;
    display_msg_t msg = { .type = DISPLAY_MSG_TEXT };
    if (text) {
        strncpy(msg.data.text, text, sizeof(msg.data.text) - 1);
    }
    if (xQueueSend(s_ctx.queue, &msg, 0) != pdTRUE)
        ESP_LOGW(TAG, "Queue full -- text dropped");
}

void display_service_set_mqtt_connected(bool connected)
{
    if (!s_ctx.queue) return;
    display_msg_t msg = { .type = DISPLAY_MSG_MQTT_STATE,
                          .data.connected = connected };
    xQueueSend(s_ctx.queue, &msg, 0);
}
