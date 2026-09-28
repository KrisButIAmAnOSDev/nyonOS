#ifndef PREEMPT_H
#define PREEMPT_H

#include <stdint.h>
#include <stdbool.h>

#define PREEMPT_CACHELINE 64

struct preempt_state {
    char pad0[PREEMPT_CACHELINE];
    uint32_t count;
    char pad1[PREEMPT_CACHELINE];
};

extern struct preempt_state preempt_state;

static inline uint32_t preempt_count(void) {
    return __atomic_load_n(&preempt_state.count, __ATOMIC_RELAXED);
}

static inline void preempt_disable(void) {
    __atomic_add_fetch(&preempt_state.count, 1, __ATOMIC_RELAXED);
}

static inline void preempt_enable(void) {
    __atomic_sub_fetch(&preempt_state.count, 1, __ATOMIC_RELAXED);
}

void preempt_init(void);

#endif
