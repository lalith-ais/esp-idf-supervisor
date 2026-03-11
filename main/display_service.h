/*
 * display_service.h  (v3.0 -- two-zone layout, Roboto Condensed Bold)
 *
 * Display layout (128x64 SSD1306):
 *
 *   rows  0-15  (16px) -- YELLOW zone: HH:MM clock, Roboto Condensed Bold 14pt
 *   rows 16-35  (20px) -- BLUE zone:   text line 1, Roboto Condensed Bold 16pt
 *   rows 36-55  (20px) -- BLUE zone:   text line 2, Roboto Condensed Bold 16pt
 *   rows 56-63  ( 8px) -- BLUE zone:   breathing room
 *
 * MQTT topics consumed:
 *   /SYS/time          payload: "HH:MM"            -- updates clock
 *   /controller/text   payload: any UTF-8 string   -- updates text zone
 *
 * Text zone behaviour:
 *   - Payload is word-wrapped to 2 lines, each centred.
 *   - Explicit '\n' in payload forces a line break.
 *   - Empty payload clears the text zone.
 *   - On MQTT disconnect both zones revert to "----" / blank.
 *
 * GPIO defaults (override in sdkconfig):
 *   CONFIG_OLED_SDA_GPIO   default 7
 *   CONFIG_OLED_SCL_GPIO   default 8
 *   CONFIG_OLED_I2C_ADDR   default 0x3C
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

/* Thread-safe, non-blocking.
 * time_str must be "HH:MM", e.g. "09:19". */
void display_service_set_time(const char *time_str);

/* Thread-safe, non-blocking.
 * text is word-wrapped to 2 lines and centred.
 * Max payload length: 63 chars. Longer strings are silently truncated.
 * Pass "" or NULL to clear the text zone. */
void display_service_set_text(const char *text);

/* Pass false on MQTT disconnect -- reverts clock to "----" and clears text. */
void display_service_set_mqtt_connected(bool connected);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_SERVICE_H */
