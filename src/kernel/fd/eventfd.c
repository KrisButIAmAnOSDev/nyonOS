#include "kernel/lib/string.h"
#include "kernel/fd/fd.h"
#include "kernel/fd/eventfd.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"
#include "abi/abi.h"

struct eventfd {
    struct kobject obj;
    uint64_t counter;
    bool semaphore;
    char chan;
};

static int64_t eventfd_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    struct eventfd *ef = (struct eventfd *)o;
    if (len < 8) return FD_EINVAL;
    if (ef->counter == 0) return FD_EAGAIN;

    uint64_t val;
    if (ef->semaphore) {
        ef->counter--;
        val = 1;
    } else {
        val = ef->counter;
        ef->counter = 0;
    }

    if (!copyout(ubuf, &val, 8)) return FD_EFAULT;
    return 8;
}

static int64_t eventfd_write_at(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    struct eventfd *ef = (struct eventfd *)o;
    if (len < 8) return FD_EINVAL;

    uint64_t add;
    if (!copyin(&add, ubuf, 8)) return FD_EFAULT;
    if (add == UINT64_MAX) return FD_EINVAL;
    if (add > UINT64_MAX - ef->counter) return FD_EINVAL;

    ef->counter += add;
    task_wake_chan(&ef->chan);
    poll_notify();
    return 8;
}

static int eventfd_wait_readable(struct kobject *o) {
    struct eventfd *ef = (struct eventfd *)o;
    for (;;) {
        if (ef->counter > 0) return 0;
        if (task_sig_pending()) return FD_EINTR;
        task_wait(&ef->chan, 0);
    }
}

static bool eventfd_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    struct eventfd *ef = (struct eventfd *)o;
    if ((events & ABI_POLLIN) && ef->counter > 0) *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static const struct kobject_ops eventfd_ops = {
    .read_at = eventfd_read_at,
    .write_at = eventfd_write_at,
    .wait_readable = eventfd_wait_readable,
    .poll_ready = eventfd_poll_read,
};

int eventfd_create(uint32_t initval, uint32_t flags, uint16_t *out_flags) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct eventfd *ef = (struct eventfd *)kmalloc(sizeof(struct eventfd));
    if (!ef) return FD_ENOMEM;
    memset(ef, 0, sizeof(struct eventfd));

    kobject_init(&ef->obj, KOBJ_EVENTFD,
                 FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, &eventfd_ops);
    ef->obj.user_bufs = true;
    ef->counter = initval;
    ef->semaphore = (flags & ABI_EFD_SEMAPHORE) != 0;

    uint16_t fflags = 0;
    if (flags & ABI_EFD_NONBLOCK) fflags |= FD_FLAG_NONBLOCK;
    if (flags & ABI_EFD_CLOEXEC) fflags |= FD_FLAG_CLOEXEC;
    if (out_flags) *out_flags = fflags;

    int fd = fd_alloc(t, &ef->obj,
                      FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, fflags);
    kobject_put(&ef->obj);
    return fd;
}
