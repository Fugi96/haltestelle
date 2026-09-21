#include "ssd1306.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "font.h"

#define SSD1306_WIDTH    128
#define SSD1306_HEIGHT   64
#define SSD1306_PAGE_HEIGHT 8
#define SSD1306_PAGES (SSD1306_HEIGHT / SSD1306_PAGE_HEIGHT)
#define SSD1306_FB_SIZE  (SSD1306_WIDTH * SSD1306_HEIGHT / 8)   // 1024
#define SSD1306_CTRL_DATA  0x40

#define CHARACTER_PX_HEIGHT_PAGES 2
#define CHARACTER_PX_WIDTH 12
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


esp_err_t ssd1306_flush(ssd1306_handle_t h) {
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit(h->i2c, &h->ctrl, 1 + SSD1306_FB_SIZE, I2C_TIMEOUT_MS);
}

void ssd1306_deinit(ssd1306_handle_t h) {
    if (h == NULL) return;
    static const uint8_t shut_down[] = { 0x00, 0xAE };
    i2c_master_transmit(h->i2c, shut_down, sizeof(shut_down), 100);
    free(h);
}

void ssd1306_clear(ssd1306_handle_t h) {
    memset(&h->fb, 0x00, SSD1306_FB_SIZE);
}

void ssd1306_set_pixel(ssd1306_handle_t h, int x, int y, bool on) {
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT)
        return;

    int index = (y / SSD1306_PAGE_HEIGHT) * SSD1306_WIDTH + x;
    uint8_t bit = 0x01 << (y % 8);

    if (on)
        h->fb[index] |= bit;
    else
        h->fb[index] &= ~bit;
}

void ssd1306_draw_number(ssd1306_handle_t h, int number, int start_page, int start_column) {
    char digits[12];
    int n = snprintf(digits, sizeof(digits), "%d", number);
    if (n <= 0) return;

    int col = start_column;
    for (int i = 0; i < n; i++) {
        char c = digits[i];
        if (c < '0' || c > '9') {
            col += CHARACTER_PX_WIDTH;
            continue;
        }
        int d = c - '0';

        for (int page = 0; page < CHARACTER_PX_HEIGHT_PAGES; page++) {
            for (int x = 0; x < CHARACTER_PX_WIDTH; x++) {
                int index = (start_page + page) * SSD1306_WIDTH + (col + x);
                if (index >= 0 && index < SSD1306_FB_SIZE)
                    h->fb[index] |= font[d][page][x];
            }
        }
        col += CHARACTER_PX_WIDTH;
    }
}

void ssd1306_draw_number_centered(ssd1306_handle_t h, int number, int start_page) {
    int width = 0;
    int n = number;
    if (n <= 0) width = 1;
    while (n != 0) {
        n /= 10;
        width++;
    }
    int px_width = width*CHARACTER_PX_WIDTH + (width - 1); // The width - 1 constant is for 1 px spaces between chars
    int x_pos = SSD1306_WIDTH/2 - px_width/2;
    ssd1306_draw_number(h, number, start_page, x_pos);
}

void ssd1306_draw_string(ssd1306_handle_t h, const char* str, int start_page, int start_column) {
    int col = start_column;

    for (int i = 0; str[i] != '\0'; i++) {
        int idx = font_index(str[i]);
        if (idx < 0) {
            col += CHARACTER_PX_WIDTH;
            continue;
        }

        for (int page = 0; page < CHARACTER_PX_HEIGHT_PAGES; page++) {
            for (int x = 0; x < CHARACTER_PX_WIDTH; x++) {
                int index = (start_page + page) * SSD1306_WIDTH + (col + x);
                if (index >= 0 && index < SSD1306_FB_SIZE)
                    h->fb[index] |= font[idx][page][x];
            }
        }
        col += CHARACTER_PX_WIDTH;
    }
}

void ssd1306_draw_string_centered(ssd1306_handle_t h, const char* str, int start_page) {
    int len = strlen(str);
    if (len == 0) return;

    int px_width = len * CHARACTER_PX_WIDTH + (len - 1);
    int x_pos = SSD1306_WIDTH / 2 - px_width / 2;
    ssd1306_draw_string(h, str, start_page, x_pos);
}

void ssd1306_draw_line(ssd1306_handle_t h, int x0, int y0, int x1, int y1, bool on) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        ssd1306_set_pixel(h, x0, y0, on);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ssd1306_draw_rectangle(ssd1306_handle_t h, int x0, int y0, int x1, int y1, bool on) {
    ssd1306_draw_line(h, x0, y0, x1, y0, on);
    ssd1306_draw_line(h, x1, y0, x1, y1, on);
    ssd1306_draw_line(h, x1, y1, x0, y1, on);
    ssd1306_draw_line(h, x0, y1, x0, y0, on);
}

void ssd1306_draw_arc(ssd1306_handle_t h, int cx, int cy, int radius, float start_deg, float end_deg, int thickness, bool on) {
    if (thickness < 1) thickness = 1;

    int outer = radius + thickness - 1;
    
    float step = 25.0f / (float) outer;

    if (step <= 0.0f) step = 1.0f;

    for (float deg = start_deg; deg <= end_deg; deg += step) {
        float rad = deg * (float)M_PI / 180.0f;
        float c = cosf(rad);
        float s = sinf(rad);

        for (int r = radius; r < radius + thickness; r++) {
            int x = cx + (int)lroundf(r * c);
            int y = cy - (int)lroundf(r * s);
            ssd1306_set_pixel(h, x, y, on);
        }
    }
}