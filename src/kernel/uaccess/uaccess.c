#include "uaccess.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/multitask/task.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/cpu/cpu.h"


extern void user_copy_bytes(void *dst, const void *src, size_t len);
extern void user_copy_str(char *dst, const char *src, size_t max);

extern char user_copy_bytes_lo[];
extern char user_copy_bytes_hi[];
extern char user_copy_str_lo[];
extern char user_copy_str_hi[];

static bool rip_in_copy_helper(uint64_t rip) {
    if (rip >= (uint64_t)(uintptr_t)user_copy_bytes_lo &&
        rip < (uint64_t)(uintptr_t)user_copy_bytes_hi) return true;
    if (rip >= (uint64_t)(uintptr_t)user_copy_str_lo &&
        rip < (uint64_t)(uintptr_t)user_copy_str_hi) return true;
    return false;
}

bool usermode_recover_copy(struct isr_frame *frame) {
    if (!*percpu_copy_active()) return false;
    if (!rip_in_copy_helper(frame->rip)) return false;

    percpu_copy_abort_set();

    frame->rip += 1;
    return true;
}

static inline void stac_if_needed(void) {
    if (cpu_smap_enabled()) __asm__ volatile("stac" ::: "memory");
}

static inline void clac_if_needed(void) {
    if (cpu_smap_enabled()) __asm__ volatile("clac" ::: "memory");
}

static struct task *ring3_task(void) {
    struct task *t = task_current();
    if (!t || !t->pml4) return NULL;
    return t;
}

static bool do_copy(void *dst, const void *src, size_t len, bool src_is_user) {
    if (len == 0) return true;
    if (!dst || !src) return false;

    struct task *t = ring3_task();

    if (!t) {
        user_copy_bytes(dst, src, len);
        return true;
    }

    uint64_t from = (uint64_t)(uintptr_t)src;
    uint64_t to = (uint64_t)(uintptr_t)dst;

    if (src_is_user) {
        if (!user_range_ok(from, len)) return false;
    } else if (from < USER_MAX && !user_range_ok(from, len)) {
        return false;
    }

    if (to < USER_MAX && !user_range_ok(to, len)) return false;

    stac_if_needed();
    __asm__ volatile("sti" ::: "memory");

    percpu_copy_begin();
    user_copy_bytes(dst, src, len);
    percpu_copy_end();

    __asm__ volatile("cli" ::: "memory");
    clac_if_needed();

    return percpu_copy_abort_get() == 0;
}

bool copyin(void *dst, const void *src, size_t len) {
    return do_copy(dst, src, len, true);
}

bool copyout(void *dst, const void *src, size_t len) {
    return do_copy(dst, src, len, false);
}

bool copyinstr(char *dst, const char *src, size_t max) {
    if (!dst || !src || max == 0) return false;

    struct task *t = ring3_task();

    if (!t) {
        size_t i = 0;
        while (i < max && src[i]) { dst[i] = src[i]; i++; }
        dst[i] = 0;
        return true;
    }

    if (!user_range_ok((uint64_t)(uintptr_t)src, 1)) return false;

    stac_if_needed();
    __asm__ volatile("sti" ::: "memory");

    percpu_copy_begin();
    user_copy_str(dst, src, max);
    bool aborted = percpu_copy_abort_get() != 0;
    percpu_copy_end();

    __asm__ volatile("cli" ::: "memory");
    clac_if_needed();

    return !aborted;
}

size_t user_valid_range(const void *p, size_t len) {
    struct task *t = task_current();
    if (!t || !t->pml4) return 0;
    if (!user_range_ok((uint64_t)(uintptr_t)p, len)) return 0;
    if (!vmm_range_present_in(t->pml4, (vaddr_t)(uintptr_t)p, len)) return 0;
    return len;
}