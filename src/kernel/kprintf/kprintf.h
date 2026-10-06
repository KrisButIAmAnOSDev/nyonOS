#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define PRINT_SERIAL 0
#define PRINT_SCREEN 1
#define PRINT_BOTH   2

#define COLOR_BLACK   0x000000
#define COLOR_WHITE   0xFFFFFF
#define COLOR_RED     0xFF0000
#define COLOR_GREEN   0x00FF00
#define COLOR_BLUE    0x0000FF
#define COLOR_CYAN    0x00FFFF
#define COLOR_MAGENTA 0xFF00FF
#define COLOR_YELLOW  0xFFFF00

#define KATTR(flags, color) ((uint32_t)(flags) | ((uint32_t)(color) << 8))

void kprintchar(char c, uint32_t attr);
bool kprintf_busy(void);
void kprintf(uint32_t attr, const char *fmt, ...);
void kvprintf(uint32_t attr, const char *fmt, va_list args);

void kprintf_attach(volatile uint32_t *fb, uint32_t width, uint32_t height);
void kprintf_home(uint32_t x, uint32_t y);
void kprintf_clear(void);

#endif
