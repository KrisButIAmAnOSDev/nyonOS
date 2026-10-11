#ifndef PREEMPT_H
#define PREEMPT_H

#include <stdint.h>
#include <stdbool.h>

static inline uint32_t preempt_count(void) {
    extern uint32_t percpu_preempt_count_read(void);
    return percpu_preempt_count_read();
}

static inline void preempt_disable(void) {
    extern void percpu_preempt_count_inc(void);
    percpu_preempt_count_inc();
}

static inline void preempt_enable(void) {
    extern void percpu_preempt_count_dec(void);
    percpu_preempt_count_dec();
}

void preempt_init(void);

#endif
