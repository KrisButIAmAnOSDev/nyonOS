#include "syscall.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/usermode.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/multitask/task.h"
#include "kernel/sync/preempt.h"
#include "kernel/fd/fd.h"

extern void isr_stub_syscall(void);

// ring-0 stack top for the SYSCALL entry stub; republished on every switch
uint64_t syscall_kstack_top;

typedef uint64_t (*syscall_handler_t)(struct isr_frame *f);

static uint64_t sys_write(struct isr_frame *f) {
    uint32_t dest = (uint32_t)f->rdi;
    uint32_t color = (uint32_t)f->rsi;
    const char *buf = (const char *)f->rdx;
    uint64_t len = f->r10;

    if (dest > PRINT_BOTH) return SYS_EINVAL;
    if (!buf || len == 0) return SYS_EINVAL;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

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

static bool ring3(struct isr_frame *f) {
    return (f->cs & 3) == 3;
}

static bool buf_writable(struct isr_frame *f, uint64_t ptr, uint64_t len) {
    if (!ring3(f)) return true;
    if (!user_range_ok(ptr, len)) return false;
    struct page_table *pml4 = task_current_pml4();
    if (!pml4) return false;
    return vmm_range_writable_in(pml4, (vaddr_t)ptr, (size_t)len);
}

static bool buf_readable(struct isr_frame *f, uint64_t ptr, uint64_t len) {
    if (!ring3(f)) return true;
    if (!user_range_ok(ptr, len)) return false;
    struct page_table *pml4 = task_current_pml4();
    if (!pml4) return false;
    return vmm_range_present_in(pml4, (vaddr_t)ptr, (size_t)len);
}

static int copy_path(struct isr_frame *f, const char *up, char *out, size_t out_max) {
    if (!ring3(f)) {
        size_t i = 0;
        while (i < out_max - 1 && up[i]) { out[i] = up[i]; i++; }
        out[i] = 0;
        return i ? 0 : -1;
    }

    struct page_table *pml4 = task_current_pml4();
    if (!pml4) return -1;

    for (size_t i = 0; i < out_max - 1; i++) {
        uint64_t a = (uint64_t)(uintptr_t)up + i;
        if (!user_range_ok(a, 1)) return -1;
        if (!vmm_range_present_in(pml4, (vaddr_t)a, 1)) return -1;
        out[i] = up[i];
        if (out[i] == 0) return 0;
    }
    return -1;
}

static uint64_t sys_open(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    const char *up = (const char *)(uintptr_t)f->rdi;
    uint32_t flags = (uint32_t)f->rsi;

    if (!up) return SYS_EFAULT;

    char path[256];
    if (copy_path(f, up, path, sizeof(path)) != 0) return SYS_EFAULT;

    int fd = fd_open(t, path, flags);
    return (uint64_t)(int64_t)fd;
}

static uint64_t sys_close(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (uint64_t)(int64_t)fd_close(t, (int)f->rdi);
}

static uint64_t sys_read(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    void *buf = (void *)(uintptr_t)f->rsi;
    uint32_t len = (uint32_t)f->rdx;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    if (!buf_writable(f, (uint64_t)(uintptr_t)buf, len)) return SYS_EFAULT;
    return (uint64_t)fd_read(t, (int)f->rdi, buf, len);
}

static uint64_t sys_fd_write(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    const void *buf = (const void *)(uintptr_t)f->rsi;
    uint32_t len = (uint32_t)f->rdx;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    if (!buf_readable(f, (uint64_t)(uintptr_t)buf, len)) return SYS_EFAULT;
    return (uint64_t)fd_write(t, (int)f->rdi, buf, len);
}

static uint64_t sys_lseek(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (uint64_t)fd_lseek(t, (int)f->rdi, (int64_t)f->rsi, (int)f->rdx);
}

static uint64_t sys_dup(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    uint32_t rights = f->rdx ? (uint32_t)f->rdx : FD_RIGHTS_ALL;
    return (uint64_t)(int64_t)fd_dup(t, (int)f->rdi, rights);
}

static syscall_handler_t handlers[SYSCALL_NR_MAX] = {
    [SYS_WRITE] = sys_write,
    [SYS_EXIT] = sys_exit,
    [SYS_OPEN] = sys_open,
    [SYS_CLOSE] = sys_close,
    [SYS_READ] = sys_read,
    [SYS_FD_WRITE] = sys_fd_write,
    [SYS_LSEEK] = sys_lseek,
    [SYS_DUP] = sys_dup,
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
