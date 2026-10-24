#include "kernel/mm/heap/heap.h"
#include "kernel/mm/vmm/vmm.h"
#include "kernel/tty/tty.h"
#include "kernel/exec/elf.h"
#include "syscall.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/uaccess/uaccess.h"
#include "kernel/multitask/task.h"
#include "kernel/sync/preempt.h"
#include "kernel/fd/fd.h"
#include "kernel/pipe/pipe.h"
#include "kernel/fs/fs.h"
#include "kernel/mm/vmm/vmspace.h"
#include "arch/x86_64/pit/pit.h"
#include "arch/x86_64/rtc/rtc.h"
#include "arch/x86_64/syscall/syscall_msr.h"

extern void isr_stub_syscall(void);


typedef uint64_t (*syscall_handler_t)(struct isr_frame *f);

static struct isr_frame *current_frame;

static bool in_user(void) {
    struct task *t = task_current();
    return t && t->pml4 != NULL;
}

static size_t path_join(struct task *t, const char *in, char *out, size_t max) {
    if (!t || !in || !out || max < 2) return 0;

    size_t n = 0;
    out[n++] = '/';

    const char *parts[2];
    parts[0] = (in[0] == '/') ? NULL : t->cwd;
    parts[1] = in;

    for (int i = 0; i < 2; i++) {
        if (!parts[i]) continue;
        const char *p = parts[i];
        while (*p) {
            while (*p == '/') p++;
            const char *st = p;
            while (*p && *p != '/') p++;
            size_t l = (size_t)(p - st);
            if (!l) break;
            if (l == 1 && st[0] == '.') continue;
            if (l == 2 && st[0] == '.' && st[1] == '.') {
                if (n > 1) {
                    n--;
                    while (n > 1 && out[n - 1] != '/') n--;
                }
                continue;
            }
            if (n + l + 2 >= max) return 0;
            if (n > 1) out[n++] = '/';
            for (size_t k = 0; k < l; k++) out[n++] = st[k];
        }
    }

    out[n] = 0;
    return n;
}

static int64_t sys_console_write(unsigned dest, unsigned attr, uintptr_t buf, unsigned len) {
    (void)attr;

    if (dest > PRINT_NONE) return SYS_EINVAL;
    if (!buf || len == 0) return SYS_EINVAL;
    if (len > SYS_MAX_IO) len = SYS_MAX_IO;

    static uint8_t iobuf[SYS_MAX_IO];
    const uint8_t *src = (const uint8_t *)buf;

    if (in_user()) {
        if (!copyin(iobuf, (const void *)buf, len)) return SYS_EFAULT;
        src = iobuf;
    }

    preempt_disable();
    for (uint64_t i = 0; i < len; i++) {
        kprintchar(src[i], dest);
    }
    preempt_enable();

    return len;
}

static int64_t fork_common(bool share_vm, bool block_parent, uintptr_t tid_out) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;

    if (!t->vs) return SYS_EINVAL;
    if (!user_valid_range((void *)tid_out, sizeof(uint32_t))) return SYS_EFAULT;

    uint32_t child_pid = 0;

    struct task *child = task_spawn_fork(t, "fork", current_frame, t->fs_base,
                                          share_vm, true, &child_pid);
    if (!child) return SYS_ENOMEM;

    uint32_t out = child_pid;
    if (!copyout((void *)tid_out, &out, sizeof(out))) return SYS_EFAULT;

    if (block_parent) {
        uint32_t saved = preempt_open_for_block();
        task_wait_vfork_child(child_pid);
        preempt_close_after_block(saved);
    }

    return (int64_t)child_pid;
}

static int64_t sys_fork(uintptr_t tid_out) {
    return fork_common(false, false, tid_out);
}

static int64_t sys_vfork(uintptr_t tid_out) {
    return fork_common(true, true, tid_out);
}

static int64_t sys_clone(uint64_t flags, uint64_t stack, uintptr_t tid_out, uint64_t tcb) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (!t->vs) return SYS_EINVAL;
    if (!user_valid_range((void *)tid_out, sizeof(uint32_t))) return SYS_EFAULT;

    if (flags & ABI_CLONE_THREAD) {
        if (!stack) return SYS_EINVAL;
        uint32_t child_pid = 0;
        struct task *child = task_spawn_fork(t, "clone", current_frame,
                                              (flags & ABI_CLONE_SETTLS) ? tcb : t->fs_base,
                                              true, true, &child_pid);
        if (!child) return SYS_ENOMEM;
        uint32_t out = child_pid;
        if (!copyout((void *)tid_out, &out, sizeof(out))) return SYS_EFAULT;
        return 0;
    }

    bool share = (flags & ABI_CLONE_VM) != 0;
    return fork_common(share, share, tid_out);
}

static int64_t sys_newthread(uintptr_t pid_out, uint64_t entry, uint64_t stack) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (!entry || !stack) return SYS_EINVAL;
    if (entry >= VMM_HIGHER_HALF || stack >= VMM_HIGHER_HALF) return SYS_EINVAL;
    if (stack & 7) return SYS_EINVAL;

    uint64_t zero = 0;
    if (!copyout(&zero, (void *)pid_out, sizeof(zero))) return SYS_EFAULT;

    struct task *child = task_spawn_thread(t, "thread", entry, stack, t->fs_base);
    if (!child) return SYS_ENOMEM;

    uint32_t pid = child->pid;
    if (!copyout((void *)pid_out, &pid, sizeof(pid))) return SYS_EFAULT;
    return 0;
}

static int64_t sys_thread_exit(void) {
    task_exit_code(0);
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}

static int64_t sys_waitpid(int32_t pid, uintptr_t status, int32_t options) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (pid < -1) return SYS_EINVAL;

    int32_t want = pid;
    if (pid == -1) want = 0;

    return (int64_t)task_wait_child(t, want, (int32_t *)status, options);
}

#define EXEC_MAX_ARGS 256
#define EXEC_MAX_PATH 512

static void exec_free_vec(char **vec) {
    if (!vec) return;
    for (size_t i = 0; vec[i]; i++) kfree(vec[i]);
    kfree(vec);
}

static char **exec_copy_vec(uintptr_t user_vec, size_t *out_n) {
    *out_n = 0;

    if (!user_vec) {
        char **v = kmalloc(sizeof(char *));
        if (!v) return NULL;
        v[0] = NULL;
        return v;
    }

    char **out = kmalloc(sizeof(char *) * (EXEC_MAX_ARGS + 1));
    if (!out) return NULL;

    size_t n = 0;
    for (;;) {
        uintptr_t up;
        if (!copyin(&up, (const void *)(user_vec + n * sizeof(uintptr_t)), sizeof(up))) {
            exec_free_vec(out);
            return NULL;
        }
        if (!up) break;
        if (n >= EXEC_MAX_ARGS) {
            exec_free_vec(out);
            return NULL;
        }

        size_t len = 0;
        char probe[2];
        if (!copyin(probe, (const void *)up, 1)) {
            exec_free_vec(out);
            return NULL;
        }
        char *s = kmalloc(EXEC_MAX_PATH);
        if (!s) {
            exec_free_vec(out);
            return NULL;
        }
        for (size_t i = 0; i + 1 < EXEC_MAX_PATH; i++) {
            char c;
            if (!copyin(&c, (const void *)(up + i), 1)) break;
            s[i] = c;
            if (!c) { s[i + 1] = 0; break; }
            len = i + 1;
        }
        s[len] = 0;
        out[n++] = s;
    }

    out[n] = NULL;
    *out_n = n;
    return out;
}

static int64_t sys_execve(uintptr_t path, uintptr_t uargv, uintptr_t uenvp) {
    struct task *t = task_current();
    struct isr_frame *frame = current_frame;

    if (!t || !in_user()) return SYS_EINVAL;
    if (!frame) return SYS_EFAULT;

    char kpath_raw[EXEC_MAX_PATH];
    char kpath[EXEC_MAX_PATH];
    if (!copyinstr(kpath_raw, (const char *)path, sizeof kpath_raw)) return SYS_EFAULT;
    if (!path_join(t, kpath_raw, kpath, sizeof kpath)) return SYS_ENAMETOOLONG;

    size_t argc = 0, envc = 0;
    char **argv = exec_copy_vec(uargv, &argc);
    if (!argv) return SYS_EINVAL;
    char **envp = exec_copy_vec(uenvp, &envc);
    if (!envp) {
        exec_free_vec(argv);
        return SYS_EINVAL;
    }

    fs_node_t node;
    if (!fs_lookup(kpath, &node)) {
        exec_free_vec(argv);
        exec_free_vec(envp);
        return SYS_ENOENT;
    }

    struct elf_image img;
    const char *err = NULL;
    if (elf_load_env(&node, &img, &err, (const char **)argv, argc,
                     (const char **)envp, envc)) {
        kprintf(PRINT_SERIAL, "exec: %s refused: %s\n", kpath, err ? err : "?");
        exec_free_vec(argv);
        exec_free_vec(envp);
        return SYS_EINVAL;
    }

    exec_free_vec(argv);
    exec_free_vec(envp);

    struct vmspace *old_vs = t->vs;
    struct page_table *old_pml4 = t->pml4;

    t->vs = img.vs;
    t->pml4 = img.vs->pml4;
    t->user_stack_phys = 0;
    t->user_stack_base = img.vs->stack_base;
    t->user_stack_top = img.vs->stack_top;
    t->user_code_phys = 0;
    t->user_code_base = 0;
    t->user_code_pages = 0;
    t->fs_base = 0;
    t->exit_code = 0;
    t->zombie = false;
    t->blocked = false;
    t->wait_chan = NULL;

    task_aspace_update();

    if (old_vs && old_vs != img.vs) vm_destroy(old_vs);
    if (old_pml4 && old_pml4 != img.vs->pml4) vmm_destroy_address_space(old_pml4);

    fd_close_cloexec(t);

    frame->rip = img.entry;
    frame->cs = 0x23;
    frame->rflags = 0x202;
    frame->rsp = img.stack_top;
    frame->ss = 0x1b;
    frame->rax = 0;

    return 0;
}

static int64_t sys_cow_ctl(int32_t on) {
    bool prev = vmm_cow_enabled();
    if (on < 0) return (int64_t)(prev ? 1 : 0);
    if (on && !vmm_set_cow(true)) return SYS_ENOSYS;
    if (!on) vmm_set_cow(false);
    return (int64_t)(prev ? 1 : 0);
}

static int64_t sys_pipe(uintptr_t fds_ptr) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;

    int fds[2];
    if (pipe_create(t, fds) != 0) return SYS_EMFILE;

    if (!copyout((void *)fds_ptr, fds, sizeof(fds))) {
        fd_close(t, fds[0]);
        fd_close(t, fds[1]);
        return SYS_EFAULT;
    }

    return 0;
}

static int copy_path(const char *up, char *out, size_t out_max) {
    if (!in_user()) {
        size_t i = 0;
        while (i < out_max && up[i]) i++;
        if (i == out_max && up[i]) return ABI_ENAMETOOLONG;
        size_t n = 0;
        while (up[n]) { out[n] = up[n]; n++; }
        out[n] = 0;
        return n ? 0 : ABI_EFAULT;
    }
    if (!copyinstr(out, up, out_max)) return ABI_EFAULT;
    size_t n = 0;
    while (n < out_max && out[n]) n++;
    if (n == out_max) return ABI_ENAMETOOLONG;
    return out[0] ? 0 : ABI_EFAULT;
}

static int copy_path_resolved(uintptr_t up, char *out, size_t out_max) {
    char raw[256];
    int cp_err = copy_path((const char *)up, raw, sizeof raw);
    if (cp_err < 0) return cp_err;
    struct task *t = task_current();
    if (!t) return ABI_EINVAL;
    if (!path_join(t, raw, out, out_max)) return ABI_ENAMETOOLONG;
    return 0;
}

static int64_t sys_chdir(uintptr_t path) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    char kpath[256];
    int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;

    fs_node_t node;
    if (!fs_lookup(kpath, &node)) return SYS_ENOENT;
    if (!node.is_dir) return SYS_ENOTDIR;

    size_t n = 0;
    while (kpath[n]) n++;
    for (size_t i = 0; i <= n; i++) t->cwd[i] = kpath[i];
    return 0;
}

static int64_t sys_getcwd(uintptr_t buf, uint64_t size) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (!buf || size == 0) return SYS_EINVAL;

    size_t n = 0;
    while (t->cwd[n]) n++;
    if (n + 1 > size) return SYS_EINVAL;
    if (!copyout((void *)buf, t->cwd, n + 1)) return SYS_EFAULT;
    return (int64_t)n;
}

static int64_t sys_mkdir(uintptr_t path, unsigned mode) {
    (void)mode;
    char kpath[256];
    int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;
    return (int64_t)fs_mkdir(kpath, NULL);
}

static int64_t sys_mkdirat(int dirfd, uintptr_t path, unsigned mode) {
    (void)dirfd;
    (void)mode;
    char kpath[256];
    int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;
    return (int64_t)fs_mkdir(kpath, NULL);
}

static int64_t sys_unlinkat(int dirfd, uintptr_t path, int flags) {
    (void)dirfd;
    char kpath[256];
    int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;
    if (flags & 0x200) return (int64_t)fs_rmdir(kpath);
    return (int64_t)fs_unlink(kpath);
}

static int64_t sys_rmdir(uintptr_t path) {
    char kpath[256];
    int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;
    return (int64_t)fs_rmdir(kpath);
}

static int64_t sys_truncate(uintptr_t path, int64_t length) {
    char kpath[256];
    int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;
    if (length < 0) return SYS_EINVAL;
    fs_node_t node;
    if (!fs_lookup(kpath, &node)) return SYS_ENOENT;
    return (int64_t)fs_node_resize(&node, (uint32_t)length);
}

static int64_t sys_ftruncate(int fd, int64_t length) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    if (length < 0) return SYS_EINVAL;
    return (int64_t)fd_truncate(t, fd, (uint64_t)length);
}

static int64_t sys_fsync(int fd) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (int64_t)fd_sync(t, fd);
}

static int64_t sys_fdatasync(int fd) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (int64_t)fd_sync(t, fd);
}

static int64_t sys_sync(void) {
    return fs_sync() ? 0 : SYS_EIO;
}

static int64_t sys_utimensat(int dirfd, uintptr_t path, uintptr_t times, int flags) {
    (void)flags;
    if (!times) return SYS_EINVAL;

    struct sys_timespec2 tv;
    if (in_user()) {
        if (!copyin(&tv, (const void *)times, sizeof(tv))) return SYS_EFAULT;
    } else {
        tv = *(const struct sys_timespec2 *)times;
    }

    if (path) {
        char kpath[256];
        int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;
        return (int64_t)fs_utimens(kpath, tv.msec);
    }

    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (int64_t)fd_utimens(t, dirfd, tv.msec);
}

static int64_t sys_poll(uintptr_t fds_ptr, uint32_t nfds, int32_t timeout,
                        uintptr_t events_ptr) {
    if (nfds > 1024) return SYS_EINVAL;

    struct sys_pollfd fds[64];
    uint32_t n = nfds > 64 ? 64 : nfds;

    for (uint32_t i = 0; i < n; i++) {
        if (!copyin(&fds[i], (const void *)(fds_ptr + i * sizeof(struct sys_pollfd)),
                    sizeof(struct sys_pollfd)))
            return SYS_EFAULT;
        fds[i].revents = 0;
    }

    struct task *t = task_current();
    if (!t) return SYS_EINVAL;

    uint64_t deadline = 0;
    if (timeout > 0) deadline = pit_get_ticks() + pit_ms_to_ticks((uint64_t)timeout);

    int32_t ready = 0;
    for (;;) {
        ready = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t rev = 0;
            if (fds[i].fd < 0) { fds[i].revents = ABI_POLLNVAL; ready++; continue; }
            if (!fd_poll_ready(t, fds[i].fd, fds[i].events, &rev)) {
                rev = 0;
            } else {
                fds[i].revents = rev;
                ready++;
            }
        }
        if (ready > 0) break;
        if (timeout == 0) break;
        if (deadline && pit_get_ticks() >= deadline) break;
        task_wait(poll_chan(), deadline);
    }

    for (uint32_t i = 0; i < n; i++)
        if (!copyout((void *)(fds_ptr + i * sizeof(struct sys_pollfd)), &fds[i],
                     sizeof(struct sys_pollfd)))
            return SYS_EFAULT;

    if (events_ptr) {
        int32_t out = ready;
        if (!copyout((void *)events_ptr, &out, sizeof(out))) return SYS_EFAULT;
    }

    return ready;
}

static int64_t sys_tcgetattr(int fd, uintptr_t attr_ptr) {
    struct sys_termios attr;
    if (!tty_termios_get(fd, &attr)) return SYS_EINVAL;
    if (!copyout((void *)attr_ptr, &attr, sizeof(attr))) return SYS_EFAULT;
    return 0;
}

static int64_t sys_tcsetattr(int fd, int act, uintptr_t attr_ptr) {
    struct sys_termios attr;
    if (!copyin(&attr, (const void *)attr_ptr, sizeof(attr))) return SYS_EFAULT;
    if (!tty_termios_set(fd, act, &attr)) return SYS_EINVAL;
    return 0;
}

static int64_t sys_tcgetwinsize(int fd, uintptr_t ws_ptr) {
    struct sys_winsize ws;
    if (!tty_winsize_get(fd, &ws)) return SYS_EINVAL;
    if (!copyout((void *)ws_ptr, &ws, sizeof(ws))) return SYS_EFAULT;
    return 0;
}

static int64_t sys_tcsetwinsize(int fd, uintptr_t ws_ptr) {
    struct sys_winsize ws;
    if (!copyin(&ws, (const void *)ws_ptr, sizeof(ws))) return SYS_EFAULT;
    if (!tty_winsize_set(fd, &ws)) return SYS_EINVAL;
    return 0;
}

static int64_t sys_ttyname(int fd, uintptr_t buf_ptr, uint64_t size) {
    if (size > 256) size = 256;
    char buf[256];
    if (!tty_name(fd, buf, (size_t)size)) return SYS_EINVAL;
    if (!copyout((void *)buf_ptr, buf, (size_t)size)) return SYS_EFAULT;
    return 0;
}

static int64_t sys_yield(void) {
    if (current_frame) current_frame->rax = 0;
    task_yield(current_frame);
    return 0;
}

static int64_t sys_exit(int code) {
    task_exit_code(code);
    for (;;) __asm__ volatile("hlt");
    __builtin_unreachable();
}


static int64_t sys_open(uintptr_t path, int flags) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;


    if (!path) return SYS_EFAULT;

    char kpath[256];
    int cp_err = copy_path_resolved(path, kpath, sizeof kpath);
    if (cp_err < 0) return cp_err;

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

    if (!in_user() || fd_uses_user_bufs(t, fd))
        return (int64_t)fd_read(t, fd, (void *)buf, len);
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

    if (!in_user() || fd_uses_user_bufs(t, fd))
        return (int64_t)fd_write(t, fd, (const void *)buf, len);
    if (!copyin(iobuf, (const void *)buf, len)) return SYS_EFAULT;
    return (int64_t)fd_write(t, fd, iobuf, len);
}

static int64_t sys_lseek(int fd, int64_t off, int whence) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (uint64_t)fd_lseek(t, (int)fd, (int64_t)off, (int)whence);
}

static int64_t sys_dup(int fd, int cloexec, unsigned rights) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;

    unsigned r = rights ? rights : FD_RIGHTS_ALL;
    return (int64_t)fd_dup(t, fd, r, cloexec ? FD_FLAG_CLOEXEC : 0);
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

    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, 0);
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
    int cp_err = copy_path_resolved(path, kpath, sizeof(kpath));
    if (cp_err < 0) return cp_err;

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

#define SYS_MMAP_LO   0x0000000100000000ULL
#define SYS_MMAP_HI   0x00007FFFFFFFF000ULL
#define SYS_FUTEX_WAIT 0
#define SYS_FUTEX_WAKE 1

static int64_t sys_mprotect(uint64_t addr, uint64_t len, unsigned prot) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (len == 0) return SYS_EINVAL;
    if (addr & (PAGE_SIZE - 1)) return SYS_EINVAL;
    if (prot & ~(ABI_MMAP_PROT_READ | ABI_MMAP_PROT_WRITE | ABI_MMAP_PROT_EXEC)) return SYS_EINVAL;

    uint8_t vprot = 0;
    if (prot & ABI_MMAP_PROT_READ) vprot |= VM_PROT_READ;
    if (prot & ABI_MMAP_PROT_WRITE) vprot |= VM_PROT_WRITE;
    if (prot & ABI_MMAP_PROT_EXEC) vprot |= VM_PROT_EXEC;

    size_t pages = (size_t)((len + PAGE_SIZE - 1) / PAGE_SIZE);

    if (!vm_protect(t->vs, addr, pages, vprot)) return SYS_ENOMEM;
    return 0;
}

static int64_t sys_mmap(uint64_t len, unsigned prot, unsigned flags) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (len == 0 || len > SYS_MMAP_HI - SYS_MMAP_LO) return SYS_EINVAL;
    if (!(flags & ABI_MMAP_ANON)) return SYS_EINVAL;
    if (prot & ~(ABI_MMAP_PROT_READ | ABI_MMAP_PROT_WRITE | ABI_MMAP_PROT_EXEC)) return SYS_EINVAL;

    uint8_t vprot = 0;
    if (prot & ABI_MMAP_PROT_READ) vprot |= VM_PROT_READ;
    if (prot & ABI_MMAP_PROT_WRITE) vprot |= VM_PROT_WRITE;
    if (prot & ABI_MMAP_PROT_EXEC) vprot |= VM_PROT_EXEC;

    size_t pages = (size_t)((len + PAGE_SIZE - 1) / PAGE_SIZE);
    size_t bytes = pages * PAGE_SIZE;

    preempt_disable();
    vaddr_t base = 0;
    if (!vm_find_free(t->vs, SYS_MMAP_LO, SYS_MMAP_HI, bytes, &base)) {
        preempt_enable();
        return SYS_ENOMEM;
    }
    if (vm_region_add(t->vs, base, base + bytes, vprot, VM_BACKING_ANON) != 0) {
        preempt_enable();
        return SYS_ENOMEM;
    }
    if (!vm_map_anon_zeroed(t->vs, base, pages, vprot, true)) {
        vm_region_remove(t->vs, base, base + bytes);
        preempt_enable();
        return SYS_ENOMEM;
    }
    preempt_enable();
    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (len == 0 || (addr & (PAGE_SIZE - 1))) return SYS_EINVAL;

    size_t pages = (size_t)((len + PAGE_SIZE - 1) / PAGE_SIZE);
    vaddr_t end = addr + (uint64_t)pages * PAGE_SIZE;
    if (end <= addr) return SYS_EINVAL;

    struct vm_region *r = vm_region_find(t->vs, addr);
    if (!r || r->backing != VM_BACKING_ANON) return SYS_EINVAL;
    if (r->start != addr || r->end != end) return SYS_EINVAL;

    preempt_disable();
    vm_free_range(t->vs, addr, end);
    vm_region_remove(t->vs, addr, end);
    preempt_enable();
    return 0;
}

static int64_t sys_clock_gettime(unsigned clk, uintptr_t ts) {
    if (!ts) return SYS_EFAULT;

    uint64_t ticks = pit_get_ticks();
    uint32_t hz = pit_frequency();
    struct sys_timespec out;

    if (clk == ABI_CLOCK_MONOTONIC) {
        out.sec = (int64_t)(ticks / hz);
        out.nsec = (int64_t)((ticks % hz) * 1000000000ULL / hz);
    } else if (clk == ABI_CLOCK_REALTIME) {
        int64_t elapsed = (int64_t)(ticks / hz);
        int64_t base = rtc_unix_time();
        out.sec = base + elapsed;
        out.nsec = (int64_t)((ticks % hz) * 1000000000ULL / hz);
    } else {
        return SYS_EINVAL;
    }

    if (in_user()) {
        if (!copyout((void *)ts, &out, sizeof(out))) return SYS_EFAULT;
    } else {
        *(struct sys_timespec *)ts = out;
    }
    return 0;
}

static int64_t sys_futex(uintptr_t addr, unsigned op, uint32_t val, uintptr_t ts) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (!addr || (addr & 3)) return SYS_EINVAL;

    if (op == SYS_FUTEX_WAIT) {
        uint64_t deadline = 0;
        if (ts) {
            struct sys_timespec kts;
            if (!copyin(&kts, (const void *)ts, sizeof(kts))) return SYS_EFAULT;
            if (kts.sec < 0 || kts.nsec < 0 || kts.nsec >= 1000000000LL) return SYS_EINVAL;
            uint64_t ms = (uint64_t)kts.sec * 1000 + ((uint64_t)kts.nsec + 999999) / 1000000;
            deadline = pit_get_ticks() + pit_ms_to_ticks(ms);
        }
        return task_futex_wait((void *)addr, addr, val, deadline);
    }
    if (op == SYS_FUTEX_WAKE) {
        task_wake_chan((void *)addr);
        return 0;
    }
    return SYS_EINVAL;
}

static int64_t sys_set_fs_base(uint64_t base) {
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (base >= VMM_HIGHER_HALF) return SYS_EINVAL;
    t->fs_base = base;
    msr_write(MSR_FS_BASE, base);
    return 0;
}

static int64_t sys_getpid(void) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return t->pid;
}

static int64_t sys_getppid(void) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return (int64_t)(int32_t)t->parent_pid;
}

static int64_t sys_getid0(void) {
    struct task *t = task_current();
    if (!t) return SYS_ENOSYS;
    return 0;
}

static int64_t sys_getuid(void) { return sys_getid0(); }
static int64_t sys_geteuid(void) { return sys_getid0(); }
static int64_t sys_getgid(void) { return sys_getid0(); }
static int64_t sys_getegid(void) { return sys_getid0(); }

static int64_t sys_getresid(uint64_t rp, uint64_t ep, uint64_t sp) {
    uint32_t *r = (uint32_t *)rp, *e = (uint32_t *)ep, *s = (uint32_t *)sp;
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    uint32_t id = 0;
    if (r && !copyout(r, &id, sizeof(id))) return SYS_EFAULT;
    if (e && !copyout(e, &id, sizeof(id))) return SYS_EFAULT;
    if (s && !copyout(s, &id, sizeof(id))) return SYS_EFAULT;
    return 0;
}

static int64_t sys_getresuid(uint64_t r, uint64_t e, uint64_t s) { return sys_getresid(r, e, s); }
static int64_t sys_getresgid(uint64_t r, uint64_t e, uint64_t s) { return sys_getresid(r, e, s); }

static char sys_hostname[64];

static int64_t sys_sethostname(uint64_t namep, uint64_t lenp) {
    const char *name = (const char *)namep;
    size_t len = (size_t)lenp;
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (len >= sizeof(sys_hostname)) len = sizeof(sys_hostname) - 1;
    if (!name) return SYS_EFAULT;

    char tmp[sizeof(sys_hostname)];
    if (!copyin(tmp, name, len)) return SYS_EFAULT;
    tmp[len] = 0;
    for (size_t i = 0; i <= len; i++) sys_hostname[i] = tmp[i];
    return 0;
}

static int64_t sys_gethostname(uint64_t namep, uint64_t lenp) {
    char *name = (char *)namep;
    size_t len = (size_t)lenp;
    struct task *t = task_current();
    if (!t || !in_user()) return SYS_EINVAL;
    if (!name) return SYS_EFAULT;
    if (len >= sizeof(sys_hostname)) len = sizeof(sys_hostname) - 1;
    if (!copyout(name, sys_hostname, len)) return SYS_EFAULT;
    if (!copyout(name + len, "", 1)) return SYS_EFAULT;
    return 0;
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

    uint32_t fflags = 0;
    if (flags & ABI_O_CLOEXEC) fflags |= FD_FLAG_CLOEXEC;
    if (flags & ABI_O_NONBLOCK) fflags |= FD_FLAG_NONBLOCK;

    int r = fd_dup2(t, (int)oldfd, (int)newfd, fflags);
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
            if (!copyout((void *)a, &pid, sizeof(pid))) return SYS_EFAULT;
            return 0;
        }
        case 0x5410: {
            uint32_t pid = 0;
            if (!copyin(&pid, (const void *)a, sizeof(pid))) return SYS_EFAULT;
            int r = fd_tty_set_fg(fd, t, pid);
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
    current_frame = frame;
    switch (frame->rax) {
        SYSCALLS
        default:
            frame->rax = SYS_ENOSYS;
            return;
    }
}
