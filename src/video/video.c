#include "font/font.h"
#include "video.h"
#include "io/serial/serial.h"

#define GLYPH_H 16
void draw_char(volatile uint32_t *fb, uint8_t c, uint32_t x, uint32_t y, uint32_t color, uint32_t width) {
    const uint8_t *font = font_8x16[c];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = font[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                if (x + col < width && y + row < 0xFFFFFFFFu) {
                    fb[(y + row) * width + (x + col)] = color;
                }
            }
        }
    }
}

void print_str(volatile uint32_t *fb, const char *str, uint32_t x, uint32_t y, uint32_t color, uint32_t width) {
    while (*str) {
        draw_char(fb, (uint8_t)*str, x, y, color, width);
        x += 8;
        if (x >= width - 8) {
            x = 0;
            y += 16;
        }
        str++;
    }
}

static volatile uint32_t *vfb;
static uint32_t vw, vh, vx, vy, vfx;

void video_attach(volatile uint32_t *fb, uint32_t width, uint32_t height) {
    vfb = fb;
    vw = width;
    vh = height;
    video_home(0, 0);
}

void video_home(uint32_t x, uint32_t y) {
    vfx = x;
    vx = x;
    vy = y;
}

void video_clear(void) {
    if (!vfb) return;
    for (uint32_t y = 0; y < vh; y++) {
        for (uint32_t x = 0; x < vw; x++) {
            vfb[y * vw + x] = 0x000000;
        }
    }
}

static void vchar(char c) {
    if (!vfb) return;
    if (c == '\n') {
        vx = vfx;
        vy += GLYPH_H;
        return;
    }
    if (c == '\r') return;
    if (c < 32 || c > 126) return;
    if (vy + GLYPH_H > vh || vx + 8 > vw) return;
    draw_char(vfb, (uint8_t)c, vx, vy, 0xFFFFFF, vw);
    vx += 8;
}

void poutc(char c) {
    serial_putchar(c);
    vchar(c);
}

void pout(const char *str) {
    while (*str) poutc(*str++);
}

void phex(uint64_t val) {
    for (int i = 15; i >= 0; i--) {
        uint8_t n = (val >> (i * 4)) & 0xF;
        poutc(n < 10 ? '0' + n : 'a' + n - 10);
    }
}
