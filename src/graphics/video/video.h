#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>
#include <stdbool.h>
#include "abi/abi.h"
void draw_char(volatile uint32_t *fb, uint8_t c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg, uint32_t width, uint32_t height);
void video_setup(uint64_t phys, uint32_t width, uint32_t height, uint32_t pitch);
bool video_query(struct sys_fb *out);
#endif
