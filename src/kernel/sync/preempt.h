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

static inline uint32_t preempt_open_for_block(void) {
    uint32_t saved = preempt_count();
    while (preempt_count() > 0) preempt_enable();
    return saved;
}

static inline void preempt_close_after_block(uint32_t saved) {
    while (preempt_count() < saved) preempt_disable();
    while (preempt_count() > saved) preempt_enable();
}

#endif
