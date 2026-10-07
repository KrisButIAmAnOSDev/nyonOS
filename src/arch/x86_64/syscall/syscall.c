#include "syscall.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/usermode.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/multitask/task.h"
#include "kernel/sync/preempt.h"
#include "kernel/fd/fd.h"
#include "kernel/fs/fs.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/syscall/syscall_msr.h"

extern void isr_stub_syscall(void);

uint64_t syscall_kstack_top;

typedef uint64_t (*syscall_handler_t)(struct isr_frame *f);

static uint64_t sys_console_write(struct isr_frame *f) {
    uint32_t dest = (uint32_t)f->rdi;
    uint32_t color = (uint32_t)f->rsi;
    const char *buf = (const char *)f->rdx;
    uint64_t len = f->r10;

    if (dest > PRINT_BOTH) return SYS_EINVAL;
    if (!buf || len == 0) return SYS_EINVAL;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    static uint8_t iobuf[SYS_MAX_IO];
    const uint8_t *src = (const uint8_t *)(uintptr_t)buf;

    if (f->cs & 3) {
        if (!copyin(iobuf, buf, len)) return SYS_EFAULT;
        src = iobuf;
    }

    uint32_t attr = KATTR(dest, color);

    preempt_disable();
    for (uint64_t i = 0; i < len; i++) {
        kprintchar(src[i], attr);
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

    if (!copyinstr(out, up, out_max)) return -1;
    return out[0] ? 0 : -1;
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

static uint64_t sys_write(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    const void *buf = (const void *)(uintptr_t)f->rsi;
    uint32_t len = (uint32_t)f->rdx;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    if (!buf_readable(f, (uint64_t)(uintptr_t)buf, len)) return SYS_EFAULT;

    if (ring3(f)) {
        static uint8_t iobuf[SYS_MAX_IO];
        if (!copyin(iobuf, buf, len)) return SYS_EFAULT;
        return (uint64_t)fd_write(t, (int)f->rdi, iobuf, len);
    }
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

static uint64_t sys_pread(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    void *buf = (void *)(uintptr_t)f->rsi;
    uint32_t len = (uint32_t)f->rdx;
    int64_t off = (int64_t)f->r10;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;
    if (off < 0) return SYS_EINVAL;
    if (!buf_writable(f, (uint64_t)(uintptr_t)buf, len)) return SYS_EFAULT;

    struct kobject *o = fd_get_checked(t, (int)f->rdi, KOBJ_TYPE_ANY, FD_RIGHT_READ);
    if (!o) return SYS_EBADF;
    if (!o->ops || !o->ops->read_at) { kobject_put(o); return SYS_EBADF; }

    int64_t r = o->ops->read_at(o, buf, len, (uint64_t)off);
    kobject_put(o);
    return (uint64_t)r;
}

static void fill_stat(struct sys_stat *out, const fs_node_t *n) {
    out->size = n->size;
    out->cluster = n->cluster;
    out->attr = n->attr;
    out->is_dir = n->is_dir ? 1 : 0;
    for (unsigned i = 0; i < SYS_MAX_NAME; i++) out->name[i] = (i < FS_MAX_NAME) ? n->name[i] : 0;
}

static uint64_t sys_fstat(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    struct sys_stat *out = (struct sys_stat *)(uintptr_t)f->rsi;
    if (!buf_writable(f, (uint64_t)(uintptr_t)out, sizeof(*out))) return SYS_EFAULT;

    struct kobject *o = fd_get_checked(t, (int)f->rdi, KOBJ_TYPE_ANY, FD_RIGHT_READ);
    if (!o) return SYS_EBADF;
    if (o->type != KOBJ_FILE) { kobject_put(o); return SYS_EBADF; }

    fill_stat(out, &((struct file *)o)->node);
    kobject_put(o);
    return 0;
}

static uint64_t sys_stat(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    const char *up = (const char *)(uintptr_t)f->rdi;
    struct sys_stat *out = (struct sys_stat *)(uintptr_t)f->rsi;
    if (!up) return SYS_EFAULT;
    if (!buf_writable(f, (uint64_t)(uintptr_t)out, sizeof(*out))) return SYS_EFAULT;

    char path[256];
    if (copy_path(f, up, path, sizeof(path)) != 0) return SYS_EFAULT;

    fs_node_t n;
    if (!fs_lookup(path, &n)) return SYS_ENOENT;

    fill_stat(out, &n);
    return 0;
}

static uint64_t sys_getdents(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    uint32_t index = (uint32_t)f->rsi;
    struct sys_dirent *out = (struct sys_dirent *)(uintptr_t)f->rdx;
    if (!buf_writable(f, (uint64_t)(uintptr_t)out, sizeof(*out))) return SYS_EFAULT;

    struct kobject *o = fd_get_checked(t, (int)f->rdi, KOBJ_TYPE_ANY, FD_RIGHT_GETDENT);
    if (!o) return SYS_EBADF;
    if (o->type != KOBJ_FILE) { kobject_put(o); return SYS_EBADF; }

    struct file *fl = (struct file *)o;
    int64_t ret = SYS_EINVAL;
    if (fl->node.is_dir) {
        fs_node_t n;
        if (fs_iterate(&fl->node, index, &n)) {
            out->size = n.size;
            out->cluster = n.cluster;
            out->attr = n.attr;
            out->is_dir = n.is_dir ? 1 : 0;
            for (unsigned i = 0; i < SYS_MAX_NAME; i++) out->name[i] = (i < FS_MAX_NAME) ? n.name[i] : 0;
            ret = 1;
        } else {
            ret = 0;
        }
    }

    kobject_put(o);
    return (uint64_t)ret;
}

static uint64_t sys_isatty(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    struct kobject *o = fd_get_checked(t, (int)f->rdi, KOBJ_TYPE_ANY, 0);
    if (!o) return SYS_EBADF;

    bool tty = (o->type == KOBJ_CONSOLE);
    kobject_put(o);
    return tty ? 1 : 0;
}

static uint64_t sys_getpid(struct isr_frame *f) {
    (void)f;
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return t->pid;
}

static uint64_t sys_sleep(struct isr_frame *f) {
    uint64_t ms = f->rdi;
    if (ms > 60000) ms = 60000;
    if (ms == 0) return 0;

    uint64_t deadline = pit_get_ticks() + pit_ms_to_ticks(ms);
    while (pit_get_ticks() < deadline) task_wait(NULL, deadline);

    return 0;
}

static uint64_t sys_debug_print(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    if (!t->debug_log) return SYS_EPERM;

    const char *buf = (const char *)(uintptr_t)f->rdi;
    uint32_t len = (uint32_t)f->rsi;
    if (len > 512) len = 512;
    if (!buf) return SYS_EFAULT;
    if (!buf_readable(f, (uint64_t)(uintptr_t)buf, len)) return SYS_EFAULT;

    for (uint32_t i = 0; i < len; i++) kprintchar(buf[i], PRINT_SERIAL);
    return len;
}

static uint64_t sys_dup2(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    int r = fd_dup2(t, (int)f->rdi, (int)f->rsi, (uint32_t)f->rdx);
    if (r < 0) return (uint64_t)(int64_t)r;
    return (uint64_t)r;
}

static uint64_t sys_fcntl(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    int fd = (int)f->rdi;
    uint32_t cmd = (uint32_t)f->rsi;
    uint32_t arg = (uint32_t)f->rdx;

    switch (cmd) {
        case 0: { int r = fd_dup_min(t, fd, arg); if (r < 0) return (uint64_t)(int64_t)r; return (uint64_t)r; }
        case 1: { bool on; if (fd_get_cloexec(t, fd, &on)) return SYS_EBADF; return on ? 1 : 0; }
        case 2: { if (fd_set_cloexec(t, fd, arg & 1)) return SYS_EBADF; return 0; }
        case 3: {
            uint16_t fl, rights;
            if (fd_get_status(t, fd, &fl) || fd_get_rights(t, fd, &rights)) return SYS_EBADF;
            uint64_t out = 0;
            if (rights & FD_RIGHT_READ) out |= FD_OPEN_READ;
            if (rights & FD_RIGHT_WRITE) out |= FD_OPEN_WRITE;
            if (fl & FD_FLAG_NONBLOCK) out |= FD_OPEN_NONBLOCK;
            return out;
        }
        case 4: {
            uint16_t fl = (arg & FD_OPEN_NONBLOCK) ? FD_FLAG_NONBLOCK : 0;
            if (fd_set_status(t, fd, fl)) return SYS_EBADF;
            return 0;
        }
        default: return SYS_EINVAL;
    }
}

static uint64_t sys_ioctl(struct isr_frame *f) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    int fd = (int)f->rdi;
    uint32_t req = (uint32_t)f->rsi;
    uint64_t arg = f->rdx;

    switch (req) {
        case 0x5401: {
            uint32_t d = 0;
            if (fd_console_dest(fd, t, &d)) return SYS_EBADF;
            return d;
        }
        case 0x5402: {
            int r = fd_console_set_dest(fd, t, (uint32_t)arg);
            if (r) return (uint64_t)(int64_t)r;
            return 0;
        }
        case 0x540F: {
            uint32_t pid = 0;
            int r = fd_tty_get_fg(fd, t, &pid);
            if (r) return (uint64_t)(int64_t)r;
            return pid;
        }
        case 0x5410: {
            if (arg > 0xFFFFFFFFULL) return SYS_EINVAL;
            int r = fd_tty_set_fg(fd, t, (uint32_t)arg);
            if (r) return (uint64_t)(int64_t)r;
            return 0;
        }
        default: return SYS_EINVAL;
    }
}

static syscall_handler_t handlers[SYSCALL_NR_MAX] = {    [SYS_READ]           = sys_read,
    [SYS_WRITE]          = sys_write,
    [SYS_PREAD]          = sys_pread,
    [SYS_OPEN]           = sys_open,
    [SYS_CLOSE]          = sys_close,
    [SYS_LSEEK]          = sys_lseek,
    [SYS_DUP]            = sys_dup,
    [SYS_FSTAT]          = sys_fstat,
    [SYS_STAT]           = sys_stat,
    [SYS_DUP2]           = sys_dup2,
    [SYS_FCNTL]          = sys_fcntl,
    [SYS_IOCTL]          = sys_ioctl,
    [SYS_GETDENTS]       = sys_getdents,
    [SYS_ISATTY]         = sys_isatty,
    [SYS_EXIT]           = sys_exit,
    [SYS_GETPID]         = sys_getpid,
    [SYS_SLEEP]          = sys_sleep,
    [SYS_CONSOLE_WRITE]  = sys_console_write,
    [SYS_DEBUG_PRINT]    = sys_debug_print,
};

void syscall_init(void) {
    idt_set_descriptor(SYSCALL_VECTOR, isr_stub_syscall, IDT_ATTR_DPL3, 0);
    kprintf(PRINT_SERIAL, "SYSCALL: vector 0x%llx installed (int gate, DPL 3)\n", (unsigned long long)SYSCALL_VECTOR);
    syscall_msr_init();
}

void syscall_dispatch(struct isr_frame *frame) {
    uint64_t nr = frame->rax;

    if (nr >= SYSCALL_NR_MAX || !handlers[nr]) {
        frame->rax = SYS_ENOSYS;
        return;
    }

    frame->rax = handlers[nr](frame);
}
