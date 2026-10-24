#include "preempt.h"
#include "kernel/panic/panic.h"
#include "arch/x86_64/cpu/cpu.h"

uint32_t percpu_preempt_count_read(void) {
    return __atomic_load_n(&cpu_self()->preempt_count, __ATOMIC_ACQUIRE);
}

void percpu_preempt_count_inc(void) {
    __atomic_add_fetch(&cpu_self()->preempt_count, 1, __ATOMIC_RELAXED);
}

void percpu_preempt_count_dec(void) {
    uint32_t prev = __atomic_fetch_sub(&cpu_self()->preempt_count, 1, __ATOMIC_RELAXED);
    if (prev == 0) {
        panic_assert("preempt_enable with no matching preempt_disable");
    }
}

void preempt_init(void) {
    __atomic_store_n(&cpu_self()->preempt_count, 0, __ATOMIC_RELAXED);
}

