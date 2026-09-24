#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>
void draw_char(volatile uint32_t *fb, uint8_t c, uint32_t x, uint32_t y, uint32_t color, uint32_t width);
void print_str(volatile uint32_t *fb, const char *str, uint32_t x, uint32_t y, uint32_t color, uint32_t width);
#endif
