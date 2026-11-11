#include "kernel/lib/string.h"
#include "kprintf.h"
#include <stdbool.h>
#include "drivers/serial/serial.h"
#include "graphics/video/video.h"
#include "kernel/sync/preempt.h"

#define GLYPH_W 8
#define GLYPH_H 16

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, fb_stride, cur_x, cur_y, home_x;
static uint32_t cur_fg = COLOR_WHITE;
static uint32_t cur_bg = COLOR_BLACK;
static bool cur_reverse;
static int ansi_state;
static char ansi_params[16];
static size_t ansi_n;
static uint32_t term_cols;
static uint32_t term_rows;
static uint8_t cell_ch[256 * 128];
static uint32_t cell_fg[256 * 128];
static uint32_t cell_bg[256 * 128];
static uint32_t cur_col, cur_row;
static uint32_t home_col;
static uint32_t scroll_top;
static uint32_t scroll_bot = UINT32_MAX;
static uint32_t saved_col, saved_row;
static uint8_t last_print;

void kprintf_attach(volatile uint32_t *framebuffer, uint32_t width, uint32_t height,
                    uint32_t stride) {
    fb = framebuffer;
    fb_w = width;
    fb_h = height;
    fb_stride = stride ? stride : width;
    term_cols = width / GLYPH_W;
    if (term_cols > 256) term_cols = 256;
    term_rows = height / GLYPH_H;
    if (term_rows > 128) term_rows = 128;
    home_col = 0;
    cur_col = 0;
    cur_row = 0;
    scroll_top = 0;
    scroll_bot = UINT32_MAX;
    cur_reverse = false;
    last_print = ' ';
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
    cur_reverse = false;
    ansi_state = 0;
}

void kprintf_clear(void) {
    kprintf_reset_colors();
    if (!fb) return;
    for (uint32_t y = 0; y < fb_h; y++) {
        for (uint32_t x = 0; x < fb_w; x++) {
            fb[y * fb_stride + x] = 0x000000;
        }
    }
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

static void draw_cell(uint32_t col, uint32_t row, uint8_t c,
                      uint32_t fg, uint32_t bg) {
    if (col >= term_cols || row >= term_rows) return;
    uint32_t idx = row * 256 + col;
    cell_ch[idx] = c;
    cell_fg[idx] = fg;
    cell_bg[idx] = bg;
    draw_char(fb, c, col * GLYPH_W, row * GLYPH_H, fg, bg, fb_stride, fb_h);
}

static void vt_scroll_region(void) {
    if (term_rows == 0) return;
    uint32_t top = scroll_top;
    uint32_t bot = (scroll_bot == UINT32_MAX) ? (term_rows - 1) : scroll_bot;
    if (top >= bot) return;
    for (uint32_t row = top; row < bot; row++) {
        memcpy(&cell_ch[row * 256], &cell_ch[(row + 1) * 256], 256);
        memcpy(&cell_fg[row * 256], &cell_fg[(row + 1) * 256], 256 * sizeof(uint32_t));
        memcpy(&cell_bg[row * 256], &cell_bg[(row + 1) * 256], 256 * sizeof(uint32_t));
        memcpy((void *)&fb[(row * GLYPH_H) * fb_stride],
               (const void *)&fb[((row + 1) * GLYPH_H) * fb_stride],
               GLYPH_H * fb_stride * sizeof(fb[0]));
    }
    for (uint32_t col = 0; col < term_cols; col++)
        draw_cell(col, bot, ' ', cur_fg, cur_bg);
    if (cur_row > bot) cur_row = bot;
}

static void vt_newline(void) {
    cur_col = home_col;
    uint32_t bot = (scroll_bot == UINT32_MAX) ? (term_rows - 1) : scroll_bot;
    if (cur_row < bot) cur_row++;
    else vt_scroll_region();
}

static void vt_clear_screen(uint32_t mode) {
    if (mode == 2) {
        for (uint32_t row = 0; row < term_rows; row++)
            for (uint32_t col = 0; col < term_cols; col++)
                draw_cell(col, row, ' ', cur_fg, cur_bg);
        cur_col = home_col;
        cur_row = 0;
        return;
    }
    if (mode == 1) {
        for (uint32_t row = 0; row <= cur_row; row++) {
            uint32_t e = (row == cur_row) ? cur_col : term_cols;
            if (e > term_cols) e = term_cols;
            for (uint32_t col = 0; col < e; col++)
                draw_cell(col, row, ' ', cur_fg, cur_bg);
        }
        return;
    }
    for (uint32_t col = cur_col; col < term_cols; col++)
        draw_cell(col, cur_row, ' ', cur_fg, cur_bg);
    for (uint32_t row = cur_row + 1; row < term_rows; row++)
        for (uint32_t col = 0; col < term_cols; col++)
            draw_cell(col, row, ' ', cur_fg, cur_bg);
}

static void vt_erase_line(uint32_t mode) {
    if (mode == 1) {
        for (uint32_t col = 0; col <= cur_col && col < term_cols; col++)
            draw_cell(col, cur_row, ' ', cur_fg, cur_bg);
        return;
    }
    if (mode == 2) {
        for (uint32_t col = 0; col < term_cols; col++)
            draw_cell(col, cur_row, ' ', cur_fg, cur_bg);
        return;
    }
    for (uint32_t col = cur_col; col < term_cols; col++)
        draw_cell(col, cur_row, ' ', cur_fg, cur_bg);
}

static void ansi_run(char final) {
    int v[8];
    size_t n = ansi_ints(v, 8);
    int p0 = n > 0 ? v[0] : -1;
    uint32_t fg = cur_reverse ? cur_bg : cur_fg;
    uint32_t bg = cur_reverse ? cur_fg : cur_bg;

    if (final == 'm') {
        if (n == 0) { cur_fg = COLOR_WHITE; cur_bg = COLOR_BLACK; cur_reverse = false; return; }
        for (size_t i = 0; i < n; i++) {
            int p = v[i];
            if (p == 0) { cur_fg = COLOR_WHITE; cur_bg = COLOR_BLACK; cur_reverse = false; }
            else if (p == 1 || p == 22) { }
            else if (p == 7) cur_reverse = true;
            else if (p == 27) cur_reverse = false;
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
    if (final == 'J') { vt_clear_screen(p0 < 0 ? 0 : (uint32_t)p0); return; }
    if (final == 'K') { vt_erase_line(p0 < 0 ? 0 : (uint32_t)p0); return; }
    if (final == 'A') {
        uint32_t step = (p0 > 1 ? (uint32_t)p0 : 1);
        cur_row = (cur_row >= step) ? cur_row - step : 0;
        return;
    }
    if (final == 'B') {
        uint32_t step = (p0 > 1 ? (uint32_t)p0 : 1);
        cur_row += step;
        if (cur_row >= term_rows) cur_row = term_rows - 1;
        return;
    }
    if (final == 'C') {
        uint32_t step = (p0 > 1 ? (uint32_t)p0 : 1);
        cur_col += step;
        if (cur_col >= term_cols) cur_col = term_cols - 1;
        return;
    }
    if (final == 'D') {
        uint32_t step = (p0 > 1 ? (uint32_t)p0 : 1);
        cur_col = (cur_col >= step) ? cur_col - step : 0;
        return;
    }
    if (final == 'E') {
        uint32_t step = (p0 > 1 ? (uint32_t)p0 : 1);
        cur_row += step;
        if (cur_row >= term_rows) cur_row = term_rows - 1;
        cur_col = home_col;
        return;
    }
    if (final == 'F') {
        uint32_t step = (p0 > 1 ? (uint32_t)p0 : 1);
        cur_row = (cur_row >= step) ? cur_row - step : 0;
        cur_col = home_col;
        return;
    }
    if (final == 'G') {
        uint32_t c = (p0 > 0) ? (uint32_t)(p0 - 1) : 0;
        cur_col = (c < term_cols) ? c : term_cols - 1;
        return;
    }
    if (final == 'd') {
        uint32_t r = (p0 > 0) ? (uint32_t)(p0 - 1) : 0;
        cur_row = (r < term_rows) ? r : term_rows - 1;
        cur_col = home_col;
        return;
    }
    if (final == 'H' || final == 'f') {
        uint32_t r = (n > 0 && v[0] > 0) ? (uint32_t)(v[0] - 1) : 0;
        uint32_t c = (n > 1 && v[1] > 0) ? (uint32_t)(v[1] - 1) : 0;
        cur_row = (r < term_rows) ? r : term_rows - 1;
        cur_col = (c < term_cols) ? c : term_cols - 1;
        return;
    }
    if (final == 'r') {
        scroll_top = (n > 0 && v[0] > 0) ? (uint32_t)(v[0] - 1) : 0;
        scroll_bot = (n > 1 && v[1] > 0) ? (uint32_t)(v[1] - 1) : (term_rows - 1);
        if (scroll_top >= term_rows) scroll_top = term_rows - 1;
        if (scroll_bot >= term_rows) scroll_bot = term_rows - 1;
        return;
    }
    if (final == 's') { saved_col = cur_col; saved_row = cur_row; return; }
    if (final == 'u') { cur_col = saved_col; cur_row = saved_row; return; }
    if (final == 'b') {
        uint32_t rep = (p0 > 0) ? (uint32_t)p0 : 1;
        for (uint32_t i = 0; i < rep; i++) {
            if (cur_col >= term_cols) vt_newline();
            draw_cell(cur_col++, cur_row, last_print, fg, bg);
        }
        return;
    }
    if (final == 'l' || final == 'h') {
        if (v[0] == 1049) {
            vt_clear_screen(2);
            scroll_top = 0;
            scroll_bot = term_rows - 1;
        }
        return;
    }
}

static void screen_char(char c) {
    if (!fb || term_cols == 0 || term_rows == 0) return;

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
    if (ansi_state == 3) {
        ansi_state = 0;
        return;
    }
    if (ansi_state == 1) {
        if (c == '[') { ansi_state = 2; ansi_n = 0; }
        else if (c == '(' || c == ')' || c == '#') ansi_state = 3;
        else ansi_state = 0;
        return;
    }
    if (c == 27) { ansi_state = 1; return; }

    if (c == '\n') { vt_newline(); return; }
    if (c == '\r') { cur_col = home_col; return; }
    if (c == '\t') {
        cur_col = ((cur_col - home_col) / 8 + 1) * 8 + home_col;
        if (cur_col >= term_cols) { cur_col = home_col; vt_newline(); }
        return;
    }
    if (c == '\b') {
        if (cur_col > home_col) cur_col--;
        return;
    }
    if (c < 32 || c > 126) return;

    uint32_t fg = cur_reverse ? cur_bg : cur_fg;
    uint32_t bg = cur_reverse ? cur_fg : cur_bg;
    draw_cell(cur_col, cur_row, (uint8_t)c, fg, bg);
    last_print = (uint8_t)c;
    cur_col++;
    if (cur_col >= term_cols) vt_newline();
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

struct fmt_sink {
    uint32_t attr;
    char *buf;
    size_t cap;
    size_t len;
};

static void sink_putc(struct fmt_sink *s, char c) {
    if (!s->buf) {
        kprintchar(c, s->attr);
        return;
    }
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void put_unsigned(struct fmt_sink *s, uint64_t v, unsigned base, bool upper,
                         int min_digits, char pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int i = 0;

    do {
        if (i < 24) buf[i++] = digits[v % base];
        v /= base;
    } while (v);

    while (i < min_digits && i < 24) buf[i++] = pad;

    while (i--) sink_putc(s, buf[i]);
}

static void put_signed(struct fmt_sink *s, int64_t v, int min_digits, char pad) {
    if (v < 0) {
        sink_putc(s, '-');
        put_unsigned(s, (uint64_t)(-(v + 1)) + 1, 10, false, min_digits, pad);
        return;
    }
    put_unsigned(s, (uint64_t)v, 10, false, min_digits, pad);
}

static void kvformat(struct fmt_sink *s, const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_putc(s, *fmt);
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
                put_signed(s, v, min_digits, pad);
                break;
            }
            case 'u': {
                uint64_t v = (longness >= 2) ? va_arg(args, unsigned long long)
                           : (longness == 1) ? (unsigned long)va_arg(args, unsigned long)
                           : (unsigned)va_arg(args, unsigned int);
                put_unsigned(s, v, 10, false, min_digits, pad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = (longness >= 2) ? va_arg(args, unsigned long long)
                           : (longness == 1) ? (unsigned long)va_arg(args, unsigned long)
                           : (unsigned)va_arg(args, unsigned int);
                put_unsigned(s, v, 16, *fmt == 'X', min_digits, pad);
                break;
            }
            case 'p': {
                sink_putc(s, '0');
                sink_putc(s, 'x');
                put_unsigned(s, (uint64_t)(uintptr_t)va_arg(args, void *), 16, false, min_digits, pad);
                break;
            }
            case 's': {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";

                size_t slen = 0;
                while (str[slen] && slen < (size_t)prec) slen++;
                for (size_t i = slen; i < (size_t)width; i++) sink_putc(s, ' ');
                for (size_t i = 0; i < slen; i++) sink_putc(s, str[i]);
                break;
            }
            case 'c':
                sink_putc(s, (char)va_arg(args, int));
                break;
            case '%':
                sink_putc(s, '%');
                break;
            default:
                sink_putc(s, '%');
                sink_putc(s, *fmt);
                break;
        }
    }
}

void kvprintf(uint32_t attr, const char *fmt, va_list args) {
    struct fmt_sink s = { .attr = attr, .buf = NULL, .cap = 0, .len = 0 };
    kvformat(&s, fmt, args);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    struct fmt_sink s = { .attr = 0, .buf = buf, .cap = size, .len = 0 };
    kvformat(&s, fmt, args);
    if (size) {
        size_t n = s.len < size ? s.len : size - 1;
        buf[n] = 0;
    }
    return (int)s.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
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

void kprintf_output_force(void) {
    __atomic_store_n(&printing, 0, __ATOMIC_RELEASE);
    preempt_disable();
    __atomic_test_and_set(&printing, __ATOMIC_ACQ_REL);
}

void kprintf(uint32_t attr, const char *fmt, ...) {
    va_list args;

    if (!kprintf_output_trylock()) return;

    va_start(args, fmt);
    kvprintf(attr, fmt, args);
    va_end(args);

    kprintf_output_unlock();
}
