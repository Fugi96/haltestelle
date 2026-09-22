#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One RGB565 pixel per entry, row by row: 5 bits red, 6 green, 5 blue.
typedef uint16_t gfx_color_t;

#define GFX_RGB(r, g, b) ((gfx_color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define GFX_BLACK        ((gfx_color_t)0x0000)
#define GFX_WHITE        GFX_RGB(255, 255, 255)
#define GFX_AMBER        GFX_RGB(255, 176, 0)
#define GFX_BUF_LEN(width, height) ((width) * (height))

typedef struct {
    gfx_color_t *buf;
    int          width;
    int          height;
} gfx_canvas_t;

void gfx_clear(gfx_canvas_t *c);

// GFX_BLACK erases.
void gfx_set_pixel(gfx_canvas_t *c, int x, int y, gfx_color_t color);
void gfx_draw_line(gfx_canvas_t *c, int x0, int y0, int x1, int y1, gfx_color_t color);
void gfx_draw_rect(gfx_canvas_t *c, int x0, int y0, int x1, int y1, gfx_color_t color);
void gfx_draw_arc(gfx_canvas_t *c, int cx, int cy, int radius, float start_deg, float end_deg, int thickness, gfx_color_t color);

// Text is UTF-8, 9 px high with y as the top row; the baseline is row y + 6.
// Characters missing from the font are drawn as '?'.
// Returns the x just past the drawn text.
int gfx_draw_string(gfx_canvas_t *c, const char *str, int x, int y, gfx_color_t color);
void gfx_draw_string_centered(gfx_canvas_t *c, const char *str, int y, gfx_color_t color);
void gfx_draw_number(gfx_canvas_t *c, int number, int x, int y, gfx_color_t color);
void gfx_draw_number_centered(gfx_canvas_t *c, int number, int y, gfx_color_t color);
// Width in pixels, including each glyph's trailing spacing.
int gfx_text_width(const char *str);

#ifdef __cplusplus
}
#endif
