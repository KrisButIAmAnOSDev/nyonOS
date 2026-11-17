#include "fs.h"

#include "kernel/kprintf/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/sync/preempt.h"
#include "kernel/sync/sync.h"
#include "arch/x86_64/pit/pit.h"

#define FLOCK_MAX 64

struct flock_rec {
    bool used;
    uint32_t pid;
    uint64_t key;
    uint8_t type;
    uint64_t start;
    uint64_t end;
};

static struct flock_rec flocks[FLOCK_MAX];
static spinlock_t flock_lock = SPINLOCK_INIT;

static bool flock_overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

int fs_lock_set(uint32_t pid, uint64_t key, int type, uint64_t start, uint64_t end, bool block) {
    for (;;) {
        lock_acquire(LOCK_FS, &flock_lock);

        bool conflict = false;
        for (uint32_t i = 0; i < FLOCK_MAX; i++) {
            struct flock_rec *r = &flocks[i];
            if (!r->used || r->pid == pid || r->key != key) continue;
            if (!flock_overlap(start, end, r->start, r->end)) continue;
            if (type == ABI_F_WRLCK || r->type == ABI_F_WRLCK) {
                conflict = true;
                break;
            }
        }

        if (!conflict) {
            for (uint32_t i = 0; i < FLOCK_MAX; i++) {
                struct flock_rec *r = &flocks[i];
                if (!r->used || r->pid != pid || r->key != key) continue;
                if (!flock_overlap(start, end, r->start, r->end)) continue;
                r->used = false;
            }
            if (type != ABI_F_UNLCK) {
                for (uint32_t i = 0; i < FLOCK_MAX; i++) {
                    if (flocks[i].used) continue;
                    flocks[i].used = true;
                    flocks[i].pid = pid;
                    flocks[i].key = key;
                    flocks[i].type = (uint8_t)type;
                    flocks[i].start = start;
                    flocks[i].end = end;
                    break;
                }
            }
            lock_release(LOCK_FS, &flock_lock);
            return 0;
        }

        lock_release(LOCK_FS, &flock_lock);
        if (!block) return ABI_EAGAIN;
        pit_sleep(5);
    }
}

int fs_lock_get(uint32_t pid, uint64_t key, int *type, uint64_t *start, uint64_t *end) {
    lock_acquire(LOCK_FS, &flock_lock);
    for (uint32_t i = 0; i < FLOCK_MAX; i++) {
        struct flock_rec *r = &flocks[i];
        if (!r->used || r->pid == pid || r->key != key) continue;
        *type = r->type;
        *start = r->start;
        *end = r->end;
        lock_release(LOCK_FS, &flock_lock);
        return 0;
    }
    *type = ABI_F_UNLCK;
    lock_release(LOCK_FS, &flock_lock);
    return 0;
}

void fs_lock_release(uint32_t pid, uint64_t key) {
    lock_acquire(LOCK_FS, &flock_lock);
    for (uint32_t i = 0; i < FLOCK_MAX; i++) {
        struct flock_rec *r = &flocks[i];
        if (!r->used || r->pid != pid) continue;
        if (key && r->key != key) continue;
        r->used = false;
    }
    lock_release(LOCK_FS, &flock_lock);
}
