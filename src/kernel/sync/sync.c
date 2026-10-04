#include "sync.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "drivers/serial/serial.h"

static uint32_t held_mask = 0;
static uint32_t lock_depth = 0;

static const char *lock_names[LOCK_COUNT] = {
    "none", "ata_lock", "sched_lock", "fd_lock", "heap_lock", "vmm_lock", "pmm_lock"
};

const char *lock_name(lock_id_t id) {
    if (id <= LOCK_NONE || id >= LOCK_COUNT) return "invalid";
    return lock_names[id];
}

void sync_init(void) {
    held_mask = 0;
    lock_depth = 0;
    preempt_init();
}

uint32_t sync_lock_depth(void) {
    return lock_depth;
}

void spin_lock(spinlock_t *l) {
    preempt_disable();
    while (atomic_flag_test_and_set_explicit(&l->f, memory_order_acquire)) {
        preempt_enable();
        __builtin_ia32_pause();
        preempt_disable();
    }
}

void spin_unlock(spinlock_t *l) {
    atomic_flag_clear_explicit(&l->f, memory_order_release);
    preempt_enable();
}

static void lock_panic(const char *what, lock_id_t a, lock_id_t b) {
    kprintf(PRINT_SERIAL, "lock ");
    kprintf(PRINT_SERIAL, "%s", lock_name(a));
    if (b != LOCK_NONE) {
        kprintf(PRINT_SERIAL, " vs ");
        kprintf(PRINT_SERIAL, "%s", lock_name(b));
    }
    kprintf(PRINT_SERIAL, ": ");
    kprintf(PRINT_SERIAL, "%s", what);
    kprintf(PRINT_SERIAL, "\n");
    panic_assert("lock invariant violated");
}

void lock_acquire(lock_id_t id, spinlock_t *l) {
    uint32_t bit = 1u << id;

    if (held_mask & bit) {
        lock_panic("recursive acquire, already held by this task", id, LOCK_NONE);
    }

    for (int j = id + 1; j < LOCK_COUNT; j++) {
        if (held_mask & (1u << j)) {
            lock_panic("acquired an outer lock while holding an inner one", id, (lock_id_t)j);
        }
    }

    if (lock_depth >= LOCK_MAX_DEPTH) {
        lock_panic("nested deeper than LOCK_MAX_DEPTH", id, LOCK_NONE);
    }

    spin_lock(l);
    held_mask |= bit;
    lock_depth++;
}

void lock_release(lock_id_t id, spinlock_t *l) {
    uint32_t bit = 1u << id;

    if (!(held_mask & bit)) {
        lock_panic("release of a lock this task does not hold", id, LOCK_NONE);
    }

    held_mask &= ~bit;
    lock_depth--;

    spin_unlock(l);
}
