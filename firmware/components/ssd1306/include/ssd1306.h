#pragma once
#include "driver/i2c_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssd1306_dev *ssd1306_handle_t;

esp_err_t ssd1306_init(i2c_master_dev_handle_t i2c, ssd1306_handle_t *h);
void ssd1306_deinit(ssd1306_handle_t h);
void ssd1306_clear(ssd1306_handle_t h);
void ssd1306_set_pixel(ssd1306_handle_t h, int x, int y, bool on);
void ssd1306_draw_number(ssd1306_handle_t h, int number, int start_page, int start_column);
void ssd1306_draw_number_centered(ssd1306_handle_t h, int number, int start_page);
void ssd1306_draw_line(ssd1306_handle_t h, int x0, int y0, int x1, int y1, bool on);
void ssd1306_draw_rectangle(ssd1306_handle_t h, int x0, int y0, int x1, int y1, bool on);
void ssd1306_draw_arc(ssd1306_handle_t h, int cx, int cy, int radius, float start_deg, float end_deg, int thickness, bool on);
void ssd1306_draw_string(ssd1306_handle_t h, const char* str, int start_page, int start_column);
void ssd1306_draw_string_centered(ssd1306_handle_t h, const char* str, int start_page);
esp_err_t ssd1306_flush(ssd1306_handle_t h);


#ifdef __cplusplus
}
#endif

