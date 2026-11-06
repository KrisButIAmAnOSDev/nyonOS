#include <string.h>
#include "kernel/fd/fd.h"
#include "kernel/fd/signalfd.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"
#include "abi/abi.h"

struct signalfd {
    struct kobject obj;
    uint64_t mask;
    char chan;
};

static struct fd_entry *signalfd_slot(struct fd_table *ft, uint32_t i) {
    return i < FD_INLINE ? &ft->inl[i] : &ft->heap[i - FD_INLINE];
}

bool task_signalfd_suppress(struct task *t, int sig) {
    if (!t || sig <= 0 || sig >= 65) return false;
    if (sig == ABI_SIGKILL || sig == ABI_SIGSTOP) return false;
    uint64_t bit = 1ULL << (sig - 1);
    if (t->sig_blocked & bit) return false;

    struct fd_table *ft = &t->fds;
    for (uint32_t i = 0; i < ft->capacity; i++) {
        struct fd_entry *e = signalfd_slot(ft, i);
        if (!e->obj || e->obj->type != KOBJ_SIGNALFD) continue;
        struct signalfd *sf = (struct signalfd *)e->obj;
        if (sf->mask & bit) {
            task_wake_chan(&sf->chan);
            poll_notify();
            return true;
        }
    }
    return false;
}

static uint64_t signalfd_match(struct signalfd *sf, struct task *t) {
    uint64_t pend = t->sig_pending & ~t->sig_blocked;
    return pend & sf->mask;
}

static int64_t signalfd_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    struct signalfd *sf = (struct signalfd *)o;
    struct task *t = task_current();
    if (!t) return FD_EINVAL;
    if (len < sizeof(struct sys_signalfd_siginfo)) return FD_EINVAL;

    uint64_t match = signalfd_match(sf, t);
    if (!match) return FD_EAGAIN;

    int s = __builtin_ctzll(match) + 1;
    t->sig_pending &= ~(1ULL << (s - 1));

    struct sys_signalfd_siginfo info;
    memset(&info, 0, sizeof info);
    info.ssi_signo = (uint32_t)s;

    if (!copyout(ubuf, &info, sizeof info)) return FD_EFAULT;
    return (int64_t)sizeof info;
}

static int signalfd_wait_readable(struct kobject *o) {
    struct signalfd *sf = (struct signalfd *)o;
    struct task *t = task_current();
    for (;;) {
        if (t && signalfd_match(sf, t)) return 0;
        if (task_sig_pending()) return FD_EINTR;
        task_wait(&sf->chan, 0);
    }
}

static bool signalfd_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    struct signalfd *sf = (struct signalfd *)o;
    struct task *t = task_current();
    if ((events & ABI_POLLIN) && t && signalfd_match(sf, t)) *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static const struct kobject_ops signalfd_ops = {
    .read_at = signalfd_read_at,
    .wait_readable = signalfd_wait_readable,
    .poll_ready = signalfd_poll_read,
};

int signalfd_create(uint64_t mask, uint32_t flags, uint16_t *out_flags) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    uint64_t forbidden = (1ULL << (ABI_SIGKILL - 1)) | (1ULL << (ABI_SIGSTOP - 1));
    if (mask & forbidden) return FD_EINVAL;

    struct signalfd *sf = (struct signalfd *)kmalloc(sizeof(struct signalfd));
    if (!sf) return FD_ENOMEM;
    memset(sf, 0, sizeof(struct signalfd));

    kobject_init(&sf->obj, KOBJ_SIGNALFD,
                 FD_RIGHT_READ | FD_RIGHT_DUP, &signalfd_ops);
    sf->obj.user_bufs = true;
    sf->mask = mask;

    uint16_t fflags = 0;
    if (flags & ABI_SFD_NONBLOCK) fflags |= FD_FLAG_NONBLOCK;
    if (flags & ABI_SFD_CLOEXEC) fflags |= FD_FLAG_CLOEXEC;
    if (out_flags) *out_flags = fflags;

    int fd = fd_alloc(t, &sf->obj,
                      FD_RIGHT_READ | FD_RIGHT_DUP, fflags);
    kobject_put(&sf->obj);
    if (fd < 0) kobject_free(&sf->obj);
    return fd;
}

int signalfd_setmask(int fd, uint64_t mask) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    uint64_t forbidden = (1ULL << (ABI_SIGKILL - 1)) | (1ULL << (ABI_SIGSTOP - 1));
    if (mask & forbidden) return FD_EINVAL;

    struct kobject *o = fd_get_checked(t, fd, KOBJ_SIGNALFD, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    struct signalfd *sf = (struct signalfd *)o;
    sf->mask = mask;
    task_wake_chan(&sf->chan);
    kobject_put(o);
    return 0;
}
