#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define PRINT_SERIAL 0
#define PRINT_SCREEN 1
#define PRINT_BOTH   2
#define PRINT_NONE   3

#define COLOR_BLACK   0x000000
#define COLOR_WHITE   0xFFFFFF

void kprintchar(char c, uint32_t attr);
bool kprintf_busy(void);
void kprintf(uint32_t attr, const char *fmt, ...);
void kvprintf(uint32_t attr, const char *fmt, va_list args);

void kprintf_attach(volatile uint32_t *fb, uint32_t width, uint32_t height);
void kprintf_home(uint32_t x, uint32_t y);
void kprintf_clear(void);
void kprintf_reset_colors(void);

#endif
