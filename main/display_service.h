/*
 * display_service.h  (v2.0 -- SSD1306 128x64 OLED)
 *
 * Renders HH:MM in Liberation Sans Bold 46pt (Arial-equivalent) on a
 * 128x64 monochrome OLED via the ESP-IDF esp_lcd panel API.
 *
 * Glyphs are pre-rendered bitmaps stored as const arrays in flash.
 * No LVGL, no FreeType, no runtime font library of any kind.
 * Rendering = memcpy of glyph columns into a 1024-byte framebuffer.
 *
 * GPIO defaults (set in sdkconfig or override here):
 *   CONFIG_OLED_SDA_GPIO   default 7
 *   CONFIG_OLED_SCL_GPIO   default 8
 *   CONFIG_OLED_I2C_ADDR   default 0x3C
 *
 * Shows "- - - -" until first /SYS/time message, and on MQTT disconnect.
 */

#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_service_start(void);
void display_service_stop(void);
bool display_service_is_running(void);

/* Thread-safe, non-blocking. time_str must be "HH:MM", e.g. "09:19". */
void display_service_set_time(const char *time_str);

/* Pass false on MQTT disconnect to revert to "- - - -". */
void display_service_set_mqtt_connected(bool connected);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_SERVICE_H */
