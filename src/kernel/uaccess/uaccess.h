#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define USER_MAX 0x0000800000000000ULL
#define USER_MIN 0x0000000000010000ULL

static inline bool user_range_ok(uint64_t addr, size_t len) {
    if (len > USER_MAX) return false;
    if (addr > USER_MAX - len) return false;
    return addr >= USER_MIN;
}

bool copyin(void *dst, const void *src, size_t len);
bool copyout(void *dst, const void *src, size_t len);
bool copyinstr(char *dst, const char *src, size_t max);
size_t user_valid_range(const void *p, size_t len);

struct isr_frame;

bool usermode_recover_copy(struct isr_frame *frame);

#endif
