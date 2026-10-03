#include "syscall.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/usermode.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/multitask/task.h"
#include "kernel/sync/preempt.h"

extern void isr_stub_syscall(void);

typedef uint64_t (*syscall_handler_t)(struct isr_frame *f);

static uint64_t sys_write(struct isr_frame *f) {
    uint32_t dest = (uint32_t)f->rdi;
    uint32_t color = (uint32_t)f->rsi;
    const char *buf = (const char *)f->rdx;
    uint64_t len = f->r10;

    if (dest > PRINT_BOTH) return SYS_EINVAL;
    if (!buf || len == 0) return SYS_EINVAL;

    if (f->cs & 3) {
        if (!user_range_ok((uint64_t)buf, len)) return SYS_EFAULT;

        struct page_table *pml4 = task_current_pml4();
        if (pml4 && !vmm_range_present_in(pml4, (vaddr_t)buf, (size_t)len)) return SYS_EFAULT;
    }

    uint32_t attr = KATTR(dest, color);

    preempt_disable();
    for (uint64_t i = 0; i < len; i++) {
        kprintchar(buf[i], attr);
    }
    preempt_enable();

    return len;
}

static uint64_t sys_exit(struct isr_frame *f) {
    task_exit_code(f->rdi);
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

static syscall_handler_t handlers[SYSCALL_NR_MAX] = {
    [SYS_WRITE] = sys_write,
    [SYS_EXIT] = sys_exit,
};

void syscall_init(void) {
    idt_set_descriptor(SYSCALL_VECTOR, isr_stub_syscall, IDT_ATTR_DPL3, 0);
    kprintf(PRINT_SERIAL, "SYSCALL: vector 0x%llx installed (int gate, DPL 3)\n", (unsigned long long)SYSCALL_VECTOR);
}

void syscall_dispatch(struct isr_frame *frame) {
    uint64_t nr = frame->rax;

    if (nr >= SYSCALL_NR_MAX || !handlers[nr]) {
        frame->rax = SYS_ENOSYS;
        return;
    }

    frame->rax = handlers[nr](frame);
}
