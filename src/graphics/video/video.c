#include "graphics/font/font.h"
#include "graphics/video/video.h"

static uint64_t video_phys;
static uint32_t video_width;
static uint32_t video_height;
static uint32_t video_pitch;

void video_setup(uint64_t phys, uint32_t width, uint32_t height, uint32_t pitch) {
    video_phys = phys;
    video_width = width;
    video_height = height;
    video_pitch = pitch;
}

bool video_query(struct sys_fb *out) {
    if (!out || !video_phys) return false;
    out->phys = video_phys;
    out->width = video_width;
    out->height = video_height;
    out->pitch = video_pitch;
    return true;
}

static inline void fb_flush(volatile uint32_t *addr) {
    __asm__ volatile("clflush %0" : : "m"(*(volatile char *)addr) : "memory");
}

void draw_char(volatile uint32_t *fb, uint8_t c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg, uint32_t width, uint32_t height) {
    const uint8_t *font = font_8x16[c];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = font[row];
        if (y + row >= height) break;
        for (int col = 0; col < 8; col++) {
            if (x + col >= width) break;
            fb[(y + row) * width + (x + col)] = (bits & (0x80 >> col)) ? fg : bg;
        }
        fb_flush(&fb[(y + row) * width + x]);
        fb_flush(&fb[(y + row) * width + x + 7]);
    }
}
