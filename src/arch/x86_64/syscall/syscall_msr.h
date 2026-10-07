#ifndef SYSCALL_MSR_H
#define SYSCALL_MSR_H

#include <stdint.h>
#include <stdbool.h>

#define MSR_EFER              0xC0000080
#define MSR_STAR              0xC0000081
#define MSR_LSTAR             0xC0000082
#define MSR_SYSCALL_SFMASK    0xC0000084

#define MSR_EFER_SCE          (1ULL << 0)

#define SYSCALL_SFMASK_VALUE  0x40700

static inline uint64_t msr_read(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void msr_write(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)(value & 0xFFFFFFFFULL)), "d"((uint32_t)(value >> 32)));
}

bool syscall_msr_init(void);

#endif
