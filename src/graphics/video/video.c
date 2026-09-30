#include "graphics/font/font.h"
#include "graphics/video/video.h"

void draw_char(volatile uint32_t *fb, uint8_t c, uint32_t x, uint32_t y, uint32_t color, uint32_t width, uint32_t height) {
    const uint8_t *font = font_8x16[c];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = font[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                if (x + col < width && y + row < height) {
                    fb[(y + row) * width + (x + col)] = color;
                }
            }
        }
    }
}
