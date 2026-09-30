#include "kprintf.h"
#include <stdbool.h>
#include "io/serial/serial.h"
#include "graphics/video/video.h"

#define GLYPH_W 8
#define GLYPH_H 16

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, cur_x, cur_y, home_x;

void kprintf_attach(volatile uint32_t *framebuffer, uint32_t width, uint32_t height) {
    fb = framebuffer;
    fb_w = width;
    fb_h = height;
    kprintf_home(0, 0);
}

void kprintf_home(uint32_t x, uint32_t y) {
    home_x = x;
    cur_x = x;
    cur_y = y;
}

void kprintf_clear(void) {
    if (!fb) return;
    for (uint32_t y = 0; y < fb_h; y++) {
        for (uint32_t x = 0; x < fb_w; x++) {
            fb[y * fb_w + x] = 0x000000;
        }
    }
}

static void screen_char(char c, uint32_t color) {
    if (!fb) return;
    if (c == '\n') {
        cur_x = home_x;
        cur_y += GLYPH_H;
        return;
    }
    if (c == '\r') return;
    if (c < 32 || c > 126) return;
    if (cur_y + GLYPH_H > fb_h || cur_x + GLYPH_W > fb_w) return;
    draw_char(fb, (uint8_t)c, cur_x, cur_y, color, fb_w, fb_h);
    cur_x += GLYPH_W;
}

static uint8_t attr_flags(uint32_t attr) { return (uint8_t)(attr & 0xFF); }
static uint32_t attr_color(uint32_t attr) {
    uint32_t c = attr >> 8;
    return c ? c : COLOR_WHITE;
}

void kprintchar(char c, uint32_t attr) {
    uint8_t flags = attr_flags(attr);
    uint32_t color = attr_color(attr);

    if (flags == PRINT_SCREEN) {
        screen_char(c, color);
        return;
    }
    if (flags == PRINT_SERIAL) {
        serial_putchar(c);
        return;
    }
    serial_putchar(c);
    screen_char(c, color);
}

static void put_unsigned(uint64_t v, unsigned base, bool upper, uint32_t attr) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int i = 0;

    if (v == 0) {
        kprintchar('0', attr);
        return;
    }

    while (v && i < 24) {
        buf[i++] = digits[v % base];
        v /= base;
    }

    while (i--) kprintchar(buf[i], attr);
}

static void put_signed(int64_t v, uint32_t attr) {
    if (v < 0) {
        kprintchar('-', attr);
        put_unsigned((uint64_t)(-(v + 1)) + 1, 10, false, attr);
        return;
    }
    put_unsigned((uint64_t)v, 10, false, attr);
}

void kvprintf(uint32_t attr, const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kprintchar(*fmt, attr);
            continue;
        }

        fmt++;
        if (!*fmt) return;

        int longness = 0;
        while (*fmt == 'l') { longness++; fmt++; }
        if (*fmt == 'z') { longness = 1; fmt++; }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t v = (longness >= 2) ? va_arg(args, long long)
                          : (longness == 1) ? (long)va_arg(args, long)
                          : (int)va_arg(args, int);
                put_signed(v, attr);
                break;
            }
            case 'u': {
                uint64_t v = (longness >= 2) ? va_arg(args, unsigned long long)
                           : (longness == 1) ? (unsigned long)va_arg(args, unsigned long)
                           : (unsigned)va_arg(args, unsigned int);
                put_unsigned(v, 10, false, attr);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = (longness >= 2) ? va_arg(args, unsigned long long)
                           : (longness == 1) ? (unsigned long)va_arg(args, unsigned long)
                           : (unsigned)va_arg(args, unsigned int);
                put_unsigned(v, 16, *fmt == 'X', attr);
                break;
            }
            case 'p': {
                kprintchar('0', attr);
                kprintchar('x', attr);
                put_unsigned((uint64_t)(uintptr_t)va_arg(args, void *), 16, false, attr);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) kprintchar(*s++, attr);
                break;
            }
            case 'c':
                kprintchar((char)va_arg(args, int), attr);
                break;
            case '%':
                kprintchar('%', attr);
                break;
            default:
                kprintchar('%', attr);
                kprintchar(*fmt, attr);
                break;
        }
    }
}

void kprintf(uint32_t attr, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(attr, fmt, args);
    va_end(args);
}
