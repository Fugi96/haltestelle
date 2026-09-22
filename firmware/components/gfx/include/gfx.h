#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 bit per pixel, row by row, GFX_STRIDE bytes per row; bit 7 of each byte is its leftmost pixel.
#define GFX_STRIDE(width)           (((width) + 7) / 8)
#define GFX_BUF_SIZE(width, height) (GFX_STRIDE(width) * (height))

typedef struct {
    uint8_t *buf;     // GFX_BUF_SIZE(width, height) bytes, owned by the caller
    int      width;
    int      height;
} gfx_canvas_t;

void gfx_clear(gfx_canvas_t *c);
// Pixels outside the canvas are ignored by all drawing functions.
void gfx_set_pixel(gfx_canvas_t *c, int x, int y, bool on);
void gfx_draw_line(gfx_canvas_t *c, int x0, int y0, int x1, int y1, bool on);
void gfx_draw_rect(gfx_canvas_t *c, int x0, int y0, int x1, int y1, bool on);
void gfx_draw_arc(gfx_canvas_t *c, int cx, int cy, int radius, float start_deg, float end_deg, int thickness, bool on);

// Text is UTF-8, 9 px high with y as the top row; the baseline is row y + 6.
// Characters missing from the font are drawn as '?'.
// Returns the x just past the drawn text.
int gfx_draw_string(gfx_canvas_t *c, const char *str, int x, int y);
void gfx_draw_string_centered(gfx_canvas_t *c, const char *str, int y);
void gfx_draw_number(gfx_canvas_t *c, int number, int x, int y);
void gfx_draw_number_centered(gfx_canvas_t *c, int number, int y);
// Width in pixels, including each glyph's trailing spacing.
int gfx_text_width(const char *str);

#ifdef __cplusplus
}
#endif
