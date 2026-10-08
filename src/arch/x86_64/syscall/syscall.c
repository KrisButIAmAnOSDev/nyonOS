#include "syscall.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/uaccess/uaccess.h"
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

static bool in_user(void) {
    struct task *t = task_current();
    return t && t->pml4 != NULL;
}

static int64_t sys_console_write(unsigned dest, unsigned attr, uintptr_t buf, unsigned len) {

    if (dest > PRINT_BOTH) return SYS_EINVAL;
    if (!buf || len == 0) return SYS_EINVAL;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    static uint8_t iobuf[SYS_MAX_IO];
    const uint8_t *src = (const uint8_t *)buf;

    if (in_user()) {
        if (!copyin(iobuf, (const void *)buf, len)) return SYS_EFAULT;
        src = iobuf;
    }

    uint32_t kattr = KATTR(dest, attr);

    preempt_disable();
    for (uint64_t i = 0; i < len; i++) {
        kprintchar(src[i], kattr);
    }
    preempt_enable();

    return len;
}

static int64_t sys_exit(int code) {
    task_exit_code(code);
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

static int copy_path(const char *up, char *out, size_t out_max) {
    if (!in_user()) {
        size_t i = 0;
        while (i < out_max - 1 && up[i]) { out[i] = up[i]; i++; }
        out[i] = 0;
        return i ? 0 : -1;
    }
    if (!copyinstr(out, up, out_max)) return -1;
    return out[0] ? 0 : -1;
}

static int64_t sys_open(uintptr_t path, int flags) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;


    if (!path) return SYS_EFAULT;

    char kpath[256];
    if (copy_path((const char *)path, kpath, sizeof(kpath)) != 0) return SYS_EFAULT;

    int fd = fd_open(t, kpath, flags);
    return (uint64_t)(int64_t)fd;
}

static int64_t sys_close(int fd) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (uint64_t)(int64_t)fd_close(t, (int)fd);
}

static int64_t sys_read(int fd, uintptr_t buf, unsigned len) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    static uint8_t iobuf[SYS_MAX_IO];

    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    if (!in_user()) return (int64_t)fd_read(t, fd, (void *)buf, len);
    if (!copyin(iobuf, (const void *)buf, len)) return SYS_EFAULT;
    int64_t r = fd_read(t, fd, iobuf, len);
    if (r > 0 && !copyout((void *)buf, iobuf, (size_t)r)) return SYS_EFAULT;
    return r;
}

static int64_t sys_write(int fd, uintptr_t buf, unsigned len) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    static uint8_t iobuf[SYS_MAX_IO];

    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    if (!in_user()) return (int64_t)fd_write(t, fd, (const void *)buf, len);
    if (!copyin(iobuf, (const void *)buf, len)) return SYS_EFAULT;
    return (int64_t)fd_write(t, fd, iobuf, len);
}

static int64_t sys_lseek(int fd, int64_t off, int whence) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (uint64_t)fd_lseek(t, (int)fd, (int64_t)off, (int)whence);
}

static int64_t sys_dup(int fd, int cloexec, unsigned rights) {
    (void)cloexec;
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    unsigned r = rights ? rights : FD_RIGHTS_ALL;
    return (int64_t)fd_dup(t, fd, r);
}

static int64_t sys_pread(int fd, uintptr_t buf, unsigned len, int64_t off) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    static uint8_t iobuf[SYS_MAX_IO];

    if (len > SYS_MAX_IO) len = SYS_MAX_IO;
    if (off < 0) return SYS_EINVAL;

    bool usr = in_user();
    void *dst = usr ? (void *)iobuf : (void *)buf;

    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, FD_RIGHT_READ);
    if (!o) return SYS_EBADF;
    if (!o->ops || !o->ops->read_at) { kobject_put(o); return SYS_EBADF; }

    int64_t r = o->ops->read_at(o, dst, len, (uint64_t)off);
    kobject_put(o);
    if (usr && r > 0 && !copyout((void *)buf, iobuf, (size_t)r)) return SYS_EFAULT;
    return r;
}

static void fill_stat(struct sys_stat *out, const fs_node_t *n) {
    out->size = n->size;
    out->cluster = n->cluster;
    out->attr = n->attr;
    out->is_dir = n->is_dir ? 1 : 0;
    for (unsigned i = 0; i < SYS_MAX_NAME; i++) out->name[i] = (i < FS_MAX_NAME) ? n->name[i] : 0;
}

static int64_t sys_fstat(int fd, uintptr_t st) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    struct sys_stat kst;
    struct sys_stat *out = in_user() ? &kst : (struct sys_stat *)st;

    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, FD_RIGHT_READ);
    if (!o) return SYS_EBADF;
    if (o->type != KOBJ_FILE) { kobject_put(o); return SYS_EBADF; }

    fill_stat(out, &((struct file *)o)->node);
    kobject_put(o);
    if (in_user() && !copyout((void *)st, out, sizeof(*out))) return SYS_EFAULT;
    return 0;
}

static int64_t sys_stat(uintptr_t path, uintptr_t st) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    if (!path) return SYS_EFAULT;

    struct sys_stat kst;
    struct sys_stat *out = in_user() ? &kst : (struct sys_stat *)st;

    char kpath[256];
    if (copy_path((const char *)path, kpath, sizeof(kpath)) != 0) return SYS_EFAULT;

    fs_node_t n;
    if (!fs_lookup(kpath, &n)) return SYS_ENOENT;

    fill_stat(out, &n);
    if (in_user() && !copyout((void *)st, out, sizeof(*out))) return SYS_EFAULT;
    return 0;
}

static int64_t sys_getdents(int fd, unsigned index, uintptr_t buf) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    struct sys_dirent kdent;
    struct sys_dirent *out = in_user() ? &kdent : (struct sys_dirent *)buf;

    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, FD_RIGHT_GETDENT);
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
    if (in_user() && ret > 0 && !copyout((void *)buf, out, sizeof(*out))) return SYS_EFAULT;
    return (int64_t)ret;
}

static int64_t sys_isatty(int fd) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    struct kobject *o = fd_get_checked(t, (int)fd, KOBJ_TYPE_ANY, 0);
    if (!o) return SYS_EBADF;

    bool tty = (o->type == KOBJ_CONSOLE);
    kobject_put(o);
    return tty ? 1 : 0;
}

static int64_t sys_getpid(void) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return t->pid;
}

static int64_t sys_sleep(unsigned ms) {
    if (ms > 60000) ms = 60000;
    if (ms == 0) return 0;

    uint64_t deadline = pit_get_ticks() + pit_ms_to_ticks(ms);
    while (pit_get_ticks() < deadline) task_wait(NULL, deadline);

    return 0;
}

static int64_t sys_debug_print(uintptr_t msg, unsigned len) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    if (!t->debug_log) return SYS_EPERM;

    if (!msg) return SYS_EFAULT;
    if (len > 512) len = 512;

    char kbuf[512];
    const char *src = (const char *)msg;
    if (in_user()) {
        if (!copyin(kbuf, (const void *)msg, len)) return SYS_EFAULT;
        src = kbuf;
    }

    for (uint32_t i = 0; i < len; i++) kprintchar(src[i], PRINT_SERIAL);
    return len;
}

static int64_t sys_dup2(int oldfd, int newfd, unsigned flags) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    int r = fd_dup2(t, (int)oldfd, (int)newfd, (uint32_t)flags);
    if (r < 0) return (uint64_t)(int64_t)r;
    return (uint64_t)r;
}

static int64_t sys_fcntl(int fd, unsigned cmd, int64_t arg) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    uint32_t a = (uint32_t)arg;

    switch (cmd) {
        case 0: { int r = fd_dup_min(t, fd, a); if (r < 0) return (uint64_t)(int64_t)r; return (uint64_t)r; }
        case 1: { bool on; if (fd_get_cloexec(t, fd, &on)) return SYS_EBADF; return on ? 1 : 0; }
        case 2: { if (fd_set_cloexec(t, fd, a & 1)) return SYS_EBADF; return 0; }
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
            uint16_t fl = (a & FD_OPEN_NONBLOCK) ? FD_FLAG_NONBLOCK : 0;
            if (fd_set_status(t, fd, fl)) return SYS_EBADF;
            return 0;
        }
        default: return SYS_EINVAL;
    }
}

static int64_t sys_ioctl(int fd, unsigned req, int64_t arg) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    uint64_t a = (uint64_t)arg;

    switch (req) {
        case 0x5401: {
            uint32_t d = 0;
            if (fd_console_dest(fd, t, &d)) return SYS_EBADF;
            return d;
        }
        case 0x5402: {
            int r = fd_console_set_dest(fd, t, (uint32_t)a);
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
            if (a > 0xFFFFFFFFULL) return SYS_EINVAL;
            int r = fd_tty_set_fg(fd, t, (uint32_t)a);
            if (r) return (uint64_t)(int64_t)r;
            return 0;
        }
        default: return SYS_EINVAL;
    }
}

#define X(name, num, ...) [SYS_##name] = sys_##name,

void syscall_init(void) {
    idt_set_descriptor(SYSCALL_VECTOR, isr_stub_syscall, IDT_ATTR_DPL3, 0);
    kprintf(PRINT_SERIAL, "SYSCALL: vector 0x%llx installed (int gate, DPL 3)\n", (unsigned long long)SYSCALL_VECTOR);
    syscall_msr_init();
}

#define FARG0
#define FARG1 (uint64_t)frame->rdi
#define FARG2 FARG1, (uint64_t)frame->rsi
#define FARG3 FARG2, (uint64_t)frame->rdx
#define FARG4 FARG3, (uint64_t)frame->r10

#undef X

#define X(name, num, ret, nargs, argnames, ...)                                        \
    case SYS_##name:                                                                  \
        frame->rax = (uint64_t)(int64_t)sys_##name(FARG##nargs);                      \
        return;

void syscall_dispatch(struct isr_frame *frame) {
    switch (frame->rax) {
        SYSCALLS
        default:
            frame->rax = SYS_ENOSYS;
            return;
    }
}
