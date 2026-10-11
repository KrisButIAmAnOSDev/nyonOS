#include "uaccess.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/multitask/task.h"
#include "arch/x86_64/idt/idt.h"
#include "arch/x86_64/cpu/cpu.h"


extern void user_copy_bytes(void *dst, const void *src, size_t len);

bool usermode_recover_copy(struct isr_frame *frame) {
    if (!*percpu_copy_active()) return false;

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

static bool ring3_task(void) {
    struct task *t = task_current();
    return t && t->pml4 != NULL;
}

static bool do_copy(void *dst, const void *src, size_t len, bool src_is_user) {
    if (len == 0) return true;
    if (!dst || !src) return false;

    if (!ring3_task()) {
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

    if (!ring3_task()) {
        size_t i = 0;
        while (i < max && src[i]) { dst[i] = src[i]; i++; }
        dst[i] = 0;
        return true;
    }


    if (!user_range_ok((uint64_t)(uintptr_t)src, 1)) return false;

    stac_if_needed();
    __asm__ volatile("sti" ::: "memory");

    percpu_copy_begin();
    for (size_t i = 0; i < max; i++) {
        if (percpu_copy_abort_get()) break;
        dst[i] = src[i];
        if (dst[i] == 0) {
            percpu_copy_end();
            __asm__ volatile("cli" ::: "memory");
            clac_if_needed();
            return true;
        }
    }
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
