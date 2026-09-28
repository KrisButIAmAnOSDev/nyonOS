#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>
void draw_char(volatile uint32_t *fb, uint8_t c, uint32_t x, uint32_t y, uint32_t color, uint32_t width);
void print_str(volatile uint32_t *fb, const char *str, uint32_t x, uint32_t y, uint32_t color, uint32_t width);

void video_attach(volatile uint32_t *fb, uint32_t width, uint32_t height);
void video_home(uint32_t x, uint32_t y);
void video_clear(void);
void poutc(char c);
void pout(const char *str);
void phex(uint64_t val);
#endif
