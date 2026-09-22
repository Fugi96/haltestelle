#pragma once
#include "driver/i2c_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssd1306_dev *ssd1306_handle_t;

esp_err_t ssd1306_init(i2c_master_dev_handle_t i2c, ssd1306_handle_t *h);
void ssd1306_deinit(ssd1306_handle_t h);

// buf is 1 bit per pixel, row by row, (width + 7) / 8 bytes per row, bit 7 of each byte leftmost.
// Returns ESP_ERR_INVALID_SIZE unless width x height is 128 x 64.
esp_err_t ssd1306_flush(ssd1306_handle_t h, const uint8_t *buf, int width, int height);

esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast);
// Off blanks the panel but keeps its RAM, so on shows the last flushed image again.
esp_err_t ssd1306_set_power(ssd1306_handle_t h, bool on);

#ifdef __cplusplus
}
#endif
