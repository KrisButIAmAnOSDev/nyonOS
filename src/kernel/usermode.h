#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>
#include <stdbool.h>

#define USER_MAX 0x0000800000000000ULL

static inline bool user_range_ok(uint64_t addr, uint64_t len) {
    if (len > USER_MAX) return false;
    if (addr > USER_MAX - len) return false;
    return addr >= 0x1000;
}

#endif
