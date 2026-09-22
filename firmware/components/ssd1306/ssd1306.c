#include "ssd1306.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SSD1306_WIDTH    128
#define SSD1306_HEIGHT   64
#define SSD1306_PAGE_HEIGHT 8
#define SSD1306_PAGES (SSD1306_HEIGHT / SSD1306_PAGE_HEIGHT)
#define SSD1306_FB_SIZE  (SSD1306_WIDTH * SSD1306_HEIGHT / 8)   // 1024
#define SSD1306_CTRL_DATA  0x40

#define I2C_TIMEOUT_MS 100

struct ssd1306_dev {
    i2c_master_dev_handle_t i2c;

    struct {
        uint8_t _pad[3];
        uint8_t ctrl;
        uint8_t fb[SSD1306_FB_SIZE];
    } __attribute__((aligned(4)));
};

_Static_assert(offsetof(struct ssd1306_dev, fb)
            == offsetof(struct ssd1306_dev, ctrl) + 1,
               "ctrl must sit immediately before fb for single-transaction flush");



esp_err_t ssd1306_init(i2c_master_dev_handle_t i2c, ssd1306_handle_t *h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;

    struct ssd1306_dev *dev = calloc(1, sizeof(struct ssd1306_dev));
    if (dev == NULL) return ESP_ERR_NO_MEM;

    dev->i2c = i2c;
    dev->ctrl = 0x40;

    const uint8_t init_sequence[] = {
        0x00, // Control Byte
        0xAE, // Set Display OFF
        0xA8, 0x3F, // Set Multiplex Ratio (0x3F = 0b00111111 = 0d63 = 64 Pixel Height)
        0xD3, 0x00, // Set Display Offset = 0
        0x40, // Set Display Start Line = 0
        0xA1, // Remap Column 127 to SEG0 (horizontal flip)
        0xC8, // Set COM Output Scan Direction Up -> Down
        0xDA ,0x12, // Set COM Pins Hardware Configuration
        0x81, 0x7F, // Set Contrast Control (0x7F = 127 (out of 0..255))
        0xA4, // Entire Display ON and resume to RAM content display (does not physically turn on display)
        0xA6, // Set Normal Display (not Inverse)
        0xD5, 0x80, // Set Display Clock Divide Ratio/Oscilator Frequency (0x80 = 1 0 0 0 | 0 0 0 0 = 8 Oscillator Freq, 1 Divide Ratio (0 + 1))
        0x8D, 0x14, // Enable charge pump regulator
        0x20, 0x00, // Addressing Mode Parameter 00b = Horizontal Addressing Mode
        0x21, 0x00, 0xFF, // Set Column Start/End address
        0x22, 0x00, 0xFF, // Set Page Start/End address
        0xAF // Set Display ON
    };

    esp_err_t err = i2c_master_transmit(i2c, init_sequence, sizeof(init_sequence), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    *h = dev;

    return ESP_OK;
}


esp_err_t ssd1306_flush(ssd1306_handle_t h, const uint8_t *buf, int width, int height) {
    if (h == NULL || buf == NULL) return ESP_ERR_INVALID_ARG;
    if (width != SSD1306_WIDTH || height != SSD1306_HEIGHT) return ESP_ERR_INVALID_SIZE;

    // Rows of horizontal bytes become pages of vertical bytes, bit 0 on top.
    int stride = (width + 7) / 8;
    memset(h->fb, 0x00, SSD1306_FB_SIZE);
    for (int y = 0; y < SSD1306_HEIGHT; y++) {
        const uint8_t *row = &buf[y * stride];
        uint8_t *page = &h->fb[(y / SSD1306_PAGE_HEIGHT) * SSD1306_WIDTH];
        uint8_t bit = 0x01 << (y % SSD1306_PAGE_HEIGHT);
        for (int x = 0; x < SSD1306_WIDTH; x++)
            if (row[x / 8] & (0x80 >> (x % 8)))
                page[x] |= bit;
    }

    return i2c_master_transmit(h->i2c, &h->ctrl, 1 + SSD1306_FB_SIZE, I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    const uint8_t cmd[] = { 0x00, 0x81, contrast };
    return i2c_master_transmit(h->i2c, cmd, sizeof(cmd), I2C_TIMEOUT_MS);
}

esp_err_t ssd1306_set_power(ssd1306_handle_t h, bool on) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    const uint8_t cmd[] = { 0x00, on ? 0xAF : 0xAE };
    return i2c_master_transmit(h->i2c, cmd, sizeof(cmd), I2C_TIMEOUT_MS);
}

void ssd1306_deinit(ssd1306_handle_t h) {
    if (h == NULL) return;
    static const uint8_t shut_down[] = { 0x00, 0xAE };
    i2c_master_transmit(h->i2c, shut_down, sizeof(shut_down), 100);
    free(h);
}
