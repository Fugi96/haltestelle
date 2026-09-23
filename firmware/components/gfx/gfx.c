#include "gfx.h"
#include "font.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UTF8_INVALID   0xFFFD
#define FALLBACK_GLYPH '?'

void gfx_clear(gfx_canvas_t *c) {
    memset(c->buf, 0x00, GFX_BUF_LEN(c->width, c->height) * sizeof(gfx_color_t));
}

void gfx_set_pixel(gfx_canvas_t *c, int x, int y, gfx_color_t color) {
    if (x < 0 || x >= c->width || y < 0 || y >= c->height)
        return;

    c->buf[y * c->width + x] = color;
}

void gfx_draw_line(gfx_canvas_t *c, int x0, int y0, int x1, int y1, gfx_color_t color) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        gfx_set_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_rect(gfx_canvas_t *c, int x0, int y0, int x1, int y1, gfx_color_t color) {
    gfx_draw_line(c, x0, y0, x1, y0, color);
    gfx_draw_line(c, x1, y0, x1, y1, color);
    gfx_draw_line(c, x1, y1, x0, y1, color);
    gfx_draw_line(c, x0, y1, x0, y0, color);
}

void gfx_draw_arc(gfx_canvas_t *c, int cx, int cy, int radius, float start_deg, float end_deg, int thickness, gfx_color_t color) {
    if (thickness < 1) thickness = 1;

    int outer = radius + thickness - 1;

    float step = 25.0f / (float) outer;

    if (step <= 0.0f) step = 1.0f;

    for (float deg = start_deg; deg <= end_deg; deg += step) {
        float rad = deg * (float)M_PI / 180.0f;
        float co = cosf(rad);
        float s = sinf(rad);

        for (int r = radius; r < radius + thickness; r++) {
            int x = cx + (int)lroundf(r * co);
            int y = cy - (int)lroundf(r * s);
            gfx_set_pixel(c, x, y, color);
        }
    }
}

// Returns the next code point and advances *s; malformed bytes come back as UTF8_INVALID.
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int extra;

    if (p[0] < 0x80)                { cp = p[0];        extra = 0; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
    else { *s += 1; return UTF8_INVALID; }

    for (int i = 1; i <= extra; i++) {
        // Also stops at the terminator, which is not a continuation byte.
        if ((p[i] & 0xC0) != 0x80) { *s += i; return UTF8_INVALID; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    return cp;
}

static const font_glyph_t *glyph_lookup(uint32_t cp) {
    int lo = 0, hi = FONT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_codepoints[mid] == cp) return &font_glyphs[mid];
        if (font_codepoints[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static const font_glyph_t *glyph_for(uint32_t cp) {
    const font_glyph_t *g = glyph_lookup(cp);
    return g ? g : glyph_lookup(FALLBACK_GLYPH);
}

int gfx_text_width(const char *str) {
    int width = 0;
    while (*str) {
        const font_glyph_t *g = glyph_for(utf8_next(&str));
        if (g) width += g->width;
    }
    return width;
}

static int draw_glyph(gfx_canvas_t *c, const font_glyph_t *g, int x, int y, gfx_color_t color) {
    for (int col = 0; col < g->width; col++)
        for (int row = 0; row < FONT_HEIGHT; row++)
            if (g->cols[col] & (1u << row))
                gfx_set_pixel(c, x + col, y + row, color);
    return x + g->width;
}

int gfx_draw_string(gfx_canvas_t *c, const char *str, int x, int y, gfx_color_t color) {
    while (*str) {
        const font_glyph_t *g = glyph_for(utf8_next(&str));
        if (g)
            x = draw_glyph(c, g, x, y, color);
    }
    return x;
}

int gfx_draw_string_ellipsized(gfx_canvas_t *c, const char *str, int x, int y, int max_width, gfx_color_t color) {
    if (gfx_text_width(str) <= max_width)
        return gfx_draw_string(c, str, x, y, color);

    const font_glyph_t *dot = glyph_lookup('.');
    int budget = max_width - (dot ? 3 * dot->width : 0);
    int at = x;

    while (*str) {
        const char *next = str;
        const font_glyph_t *g = glyph_for(utf8_next(&next));
        if (g == NULL) {
            str = next;
            continue;
        }
        if (at - x + g->width > budget)
            break;
        at = draw_glyph(c, g, at, y, color);
        str = next;
    }

    for (int i = 0; i < 3 && dot; i++)
        at = draw_glyph(c, dot, at, y, color);
    return at;
}

void gfx_draw_string_centered(gfx_canvas_t *c, const char *str, int y, gfx_color_t color) {
    gfx_draw_string(c, str, (c->width - gfx_text_width(str)) / 2, y, color);
}

void gfx_draw_number(gfx_canvas_t *c, int number, int x, int y, gfx_color_t color) {
    char digits[12];
    snprintf(digits, sizeof(digits), "%d", number);
    gfx_draw_string(c, digits, x, y, color);
}

void gfx_draw_number_centered(gfx_canvas_t *c, int number, int y, gfx_color_t color) {
    char digits[12];
    snprintf(digits, sizeof(digits), "%d", number);
    gfx_draw_string_centered(c, digits, y, color);
}
