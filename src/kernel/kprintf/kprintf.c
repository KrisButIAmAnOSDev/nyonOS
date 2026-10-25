#include "kprintf.h"
#include <stdbool.h>
#include "drivers/serial/serial.h"
#include "graphics/video/video.h"
#include "kernel/sync/preempt.h"

#define GLYPH_W 8
#define GLYPH_H 16

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, cur_x, cur_y, home_x;
static uint32_t cur_fg = COLOR_WHITE;
static uint32_t cur_bg = COLOR_BLACK;
static int ansi_state;
static char ansi_params[16];
static size_t ansi_n;

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

void kprintf_reset_colors(void) {
    cur_fg = COLOR_WHITE;
    cur_bg = COLOR_BLACK;
    ansi_state = 0;
}

void kprintf_clear(void) {
    kprintf_reset_colors();
    if (!fb) return;
    for (uint32_t y = 0; y < fb_h; y++) {
        for (uint32_t x = 0; x < fb_w; x++) {
            fb[y * fb_w + x] = 0x000000;
        }
    }
}

static void screen_scroll(void) {
    if (!fb || GLYPH_H >= fb_h) return;

    uint32_t keep = fb_h - GLYPH_H;
    for (uint32_t y = 0; y < keep; y++) {
        for (uint32_t x = 0; x < fb_w; x++) {
            fb[y * fb_w + x] = fb[(y + GLYPH_H) * fb_w + x];
        }
    }
    for (uint32_t y = keep; y < fb_h; y++) {
        for (uint32_t x = 0; x < fb_w; x++) fb[y * fb_w + x] = 0;
    }

    if (cur_y >= fb_h) cur_y = keep;
}

static void screen_newline(void) {
    cur_x = home_x;
    cur_y += GLYPH_H;
    if (cur_y + GLYPH_H > fb_h) screen_scroll();
}

static uint32_t ansi_color(uint32_t idx) {
    static const uint32_t normal[8] = {
        0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
        0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA
    };
    static const uint32_t bright[8] = {
        0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
        0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
    };
    if (idx < 8) return normal[idx];
    if (idx < 16) return bright[idx - 8];
    if (idx < 232) {
        uint32_t i = idx - 16;
        uint32_t lvl[6] = { 0, 95, 135, 175, 215, 255 };
        uint32_t r = lvl[(i / 36) % 6];
        uint32_t g = lvl[(i / 6) % 6];
        uint32_t b = lvl[i % 6];
        return (r << 16) | (g << 8) | b;
    }
    if (idx < 256) {
        uint32_t v = 8 + (idx - 232) * 10;
        return (v << 16) | (v << 8) | v;
    }
    return COLOR_WHITE;
}

static size_t ansi_ints(int *v, size_t max) {
    size_t n = 0;
    long cur = -1;
    for (size_t i = 0; i < ansi_n; i++) {
        char c = ansi_params[i];
        if (c >= '0' && c <= '9') {
            if (cur < 0) cur = 0;
            cur = cur * 10 + (c - '0');
        } else if (c == ';') {
            if (n < max) v[n++] = (int)cur;
            cur = -1;
        }
    }
    if (cur >= 0 && n < max) v[n++] = (int)cur;
    return n;
}

static void ansi_erase_range(uint32_t x0, uint32_t x1) {
    if (!fb) return;
    for (uint32_t x = x0; x < x1 && x < fb_w; x++)
        for (int row = 0; row < GLYPH_H && cur_y + (uint32_t)row < fb_h; row++)
            fb[(cur_y + (uint32_t)row) * fb_w + x] = cur_bg;
}

static void ansi_run(char final) {
    int v[8];
    size_t n = ansi_ints(v, 8);
    int p0 = n > 0 ? v[0] : -1;
    uint32_t step = (p0 > 1 ? (uint32_t)p0 : 1) * GLYPH_H;

    if (final == 'm') {
        if (n == 0) { cur_fg = COLOR_WHITE; cur_bg = COLOR_BLACK; return; }
        for (size_t i = 0; i < n; i++) {
            int p = v[i];
            if (p == 0) { cur_fg = COLOR_WHITE; cur_bg = COLOR_BLACK; }
            else if (p == 39) cur_fg = COLOR_WHITE;
            else if (p == 49) cur_bg = COLOR_BLACK;
            else if (p >= 30 && p <= 37) cur_fg = ansi_color((uint32_t)(p - 30));
            else if (p >= 90 && p <= 97) cur_fg = ansi_color((uint32_t)(p - 82));
            else if (p >= 40 && p <= 47) cur_bg = ansi_color((uint32_t)(p - 40));
            else if (p >= 100 && p <= 107) cur_bg = ansi_color((uint32_t)(p - 92));
            else if (p == 38 && i + 2 < n && v[i + 1] == 5) { cur_fg = ansi_color((uint32_t)v[i + 2]); i += 2; }
            else if (p == 48 && i + 2 < n && v[i + 1] == 5) { cur_bg = ansi_color((uint32_t)v[i + 2]); i += 2; }
        }
        return;
    }
    if (final == 'J') {
        if (p0 == 2) {
            kprintf_clear();
            cur_x = home_x;
            cur_y = 0;
        }
        return;
    }
    if (final == 'K') {
        if (p0 == 1) ansi_erase_range(home_x, cur_x + GLYPH_W);
        else if (p0 == 2) ansi_erase_range(home_x, fb_w);
        else ansi_erase_range(cur_x, fb_w);
        return;
    }
    if (final == 'A') {
        if (cur_y >= step) cur_y -= step;
        return;
    }
    if (final == 'B') {
        cur_y += step;
        if (cur_y + GLYPH_H > fb_h) cur_y = fb_h - GLYPH_H;
        return;
    }
    if (final == 'C') {
        uint32_t hstep = (p0 > 1 ? (uint32_t)p0 : 1) * GLYPH_W;
        cur_x += hstep;
        if (cur_x + GLYPH_W > fb_w) cur_x = fb_w - GLYPH_W;
        return;
    }
    if (final == 'D') {
        uint32_t hstep = (p0 > 1 ? (uint32_t)p0 : 1) * GLYPH_W;
        if (cur_x >= home_x + hstep) cur_x -= hstep;
        else cur_x = home_x;
        return;
    }
    if (final == 'G') {
        if (p0 >= 1) cur_x = home_x + (uint32_t)(p0 - 1) * GLYPH_W;
        if (cur_x + GLYPH_W > fb_w) cur_x = fb_w - GLYPH_W;
        return;
    }
    if (final == 'H' || final == 'f') {
        cur_x = home_x;
        cur_y = 0;
        return;
    }
}

static void screen_char(char c) {
    if (!fb) return;

    if (ansi_state == 2) {
        if (c >= 0x40 && c <= 0x7e) {
            ansi_state = 0;
            ansi_params[ansi_n] = 0;
            ansi_run(c);
        } else if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
            if (ansi_n < sizeof ansi_params - 1) ansi_params[ansi_n++] = c;
        } else {
            ansi_state = 0;
        }
        return;
    }
    if (ansi_state == 1) {
        if (c == '[') { ansi_state = 2; ansi_n = 0; }
        else ansi_state = 0;
        return;
    }
    if (c == 27) { ansi_state = 1; return; }

    if (c == '\n') { screen_newline(); return; }
    if (c == '\r') { cur_x = home_x; return; }
    if (c == '\t') {
        uint32_t col = (cur_x - home_x) / GLYPH_W;
        col = (col + 8) & ~(uint32_t)7;
        cur_x = home_x + col * GLYPH_W;
        if (cur_x + GLYPH_W > fb_w) screen_newline();
        return;
    }

    if (c == '\b') {
        if (cur_x >= home_x + GLYPH_W) {
            cur_x -= GLYPH_W;
            draw_char(fb, ' ', cur_x, cur_y, cur_bg, cur_bg, fb_w, fb_h);
        }
        return;
    }

    if (c < 32 || c > 126) return;

    if (cur_x + GLYPH_W > fb_w) screen_newline();
    if (cur_y + GLYPH_H > fb_h) return;

    draw_char(fb, (uint8_t)c, cur_x, cur_y, cur_fg, cur_bg, fb_w, fb_h);
    cur_x += GLYPH_W;
}

static uint8_t attr_flags(uint32_t attr) { return (uint8_t)(attr & 0xFF); }

void kprintchar(char c, uint32_t attr) {
    uint8_t flags = attr_flags(attr);

    if (flags == PRINT_SCREEN) {
        screen_char(c);
        return;
    }
    if (flags == PRINT_SERIAL) {
        serial_putchar(c);
        return;
    }
    serial_putchar(c);
    screen_char(c);
}

static void put_unsigned(uint64_t v, unsigned base, bool upper, uint32_t attr, int min_digits, char pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int i = 0;

    if (v == 0) {
        kprintchar('0', attr);
        return;
    }

    do {
        if (i < 24) buf[i++] = digits[v % base];
        v /= base;
    } while (v);

    while (i < min_digits && i < 24) buf[i++] = pad;

    while (i--) kprintchar(buf[i], attr);
}

static void put_signed(int64_t v, uint32_t attr, int min_digits, char pad) {
    if (v < 0) {
        kprintchar('-', attr);
        put_unsigned((uint64_t)(-(v + 1)) + 1, 10, false, attr, min_digits, pad);
        return;
    }
    put_unsigned((uint64_t)v, 10, false, attr, min_digits, pad);
}

void kvprintf(uint32_t attr, const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kprintchar(*fmt, attr);
            continue;
        }

        fmt++;
        if (!*fmt) return;

        int prec = -1;
        if (*fmt == '*') {
            fmt++;
            prec = va_arg(args, int);
            if (prec < 0) prec = -1;
        } else if (*fmt == '.') {
            fmt++;

            if (*fmt == '*') {
                fmt++;
                prec = va_arg(args, int);
                if (prec < 0) prec = -1;
            } else {
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
            }
        }

        int longness = 0;
        while (*fmt == 'l') { longness++; fmt++; }
        if (*fmt == 'z') { longness = 1; fmt++; }

        int zero_pad = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        int min_digits = prec > 0 ? prec : 0;
        if (width > min_digits) min_digits = width;
        char pad = zero_pad ? '0' : ' ';

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t v = (longness >= 2) ? va_arg(args, long long)
                          : (longness == 1) ? (long)va_arg(args, long)
                          : (int)va_arg(args, int);
                put_signed(v, attr, min_digits, pad);
                break;
            }
            case 'u': {
                uint64_t v = (longness >= 2) ? va_arg(args, unsigned long long)
                           : (longness == 1) ? (unsigned long)va_arg(args, unsigned long)
                           : (unsigned)va_arg(args, unsigned int);
                put_unsigned(v, 10, false, attr, min_digits, pad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = (longness >= 2) ? va_arg(args, unsigned long long)
                           : (longness == 1) ? (unsigned long)va_arg(args, unsigned long)
                           : (unsigned)va_arg(args, unsigned int);
                put_unsigned(v, 16, *fmt == 'X', attr, min_digits, pad);
                break;
            }
            case 'p': {
                kprintchar('0', attr);
                kprintchar('x', attr);
                put_unsigned((uint64_t)(uintptr_t)va_arg(args, void *), 16, false, attr, min_digits, pad);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";

                size_t slen = 0;
                while (s[slen] && slen < (size_t)prec) slen++;
                for (size_t i = slen; i < (size_t)width; i++) kprintchar(' ', attr);
                for (size_t i = 0; i < slen; i++) kprintchar(s[i], attr);
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

static volatile uint32_t printing;

bool kprintf_busy(void) {
    return __atomic_load_n(&printing, __ATOMIC_ACQUIRE) != 0;
}

bool kprintf_output_trylock(void) {
    preempt_disable();
    if (__atomic_test_and_set(&printing, __ATOMIC_ACQ_REL)) {
        preempt_enable();
        return false;
    }
    return true;
}

void kprintf_output_lock(void) {
    for (;;) {
        preempt_disable();
        if (!__atomic_test_and_set(&printing, __ATOMIC_ACQ_REL)) return;
        preempt_enable();
        __builtin_ia32_pause();
    }
}

void kprintf_output_unlock(void) {
    __atomic_clear(&printing, __ATOMIC_RELEASE);
    preempt_enable();
}

void kprintf(uint32_t attr, const char *fmt, ...) {
    va_list args;

    if (!kprintf_output_trylock()) return;

    va_start(args, fmt);
    kvprintf(attr, fmt, args);
    va_end(args);

    kprintf_output_unlock();
}
