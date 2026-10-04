#ifndef SYNC_H
#define SYNC_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include "preempt.h"

#define LOCK_CACHELINE 64
#define LOCK_MAX_DEPTH 8

typedef enum {
    LOCK_NONE = 0,
    LOCK_ATA,
    LOCK_SCHED,
    LOCK_FD,
    LOCK_HEAP,
    LOCK_VMM,
    LOCK_PMM,
    LOCK_COUNT
} lock_id_t;

typedef struct {
    atomic_flag f;
    char pad[LOCK_CACHELINE - sizeof(atomic_flag)];
} spinlock_t;

#define SPINLOCK_INIT { ATOMIC_FLAG_INIT, {0} }

void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);

void lock_acquire(lock_id_t id, spinlock_t *l);
void lock_release(lock_id_t id, spinlock_t *l);

uint32_t sync_lock_depth(void);
void sync_init(void);

const char *lock_name(lock_id_t id);

#endif
