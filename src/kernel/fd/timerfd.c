#include "kernel/lib/string.h"
#include "kernel/fd/fd.h"
#include "kernel/fd/timerfd.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"
#include "arch/x86_64/pit/pit.h"
#include "abi/abi.h"

struct timerfd {
    struct kobject obj;
    uint64_t period_ticks;
    uint64_t next_tick;
    uint64_t expirations;
    char chan;
    struct timerfd *tnext;
};

static struct timerfd *timerfd_active;
static spinlock_t timerfd_lock = SPINLOCK_INIT;

static uint64_t ns_to_ms(uint64_t ns) {
    return (ns + 999999) / 1000000;
}

static uint64_t ms_to_ns(uint64_t ms) {
    return ms * 1000000;
}

static void timerfd_list_add(struct timerfd *tf) {
    tf->tnext = timerfd_active;
    timerfd_active = tf;
}

static void timerfd_list_del(struct timerfd *tf) {
    struct timerfd **pp = &timerfd_active;
    while (*pp) {
        if (*pp == tf) {
            *pp = tf->tnext;
            break;
        }
        pp = &(*pp)->tnext;
    }
    tf->tnext = NULL;
}

static int64_t timerfd_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    struct timerfd *tf = (struct timerfd *)o;
    if (len < 8) return FD_EINVAL;
    if (!tf->expirations) return FD_EAGAIN;

    uint64_t n = tf->expirations;
    tf->expirations = 0;
    if (!copyout(ubuf, &n, 8)) return FD_EFAULT;
    return 8;
}

static int timerfd_wait_readable(struct kobject *o) {
    struct timerfd *tf = (struct timerfd *)o;
    for (;;) {
        if (tf->expirations) return 0;
        if (task_sig_pending()) return FD_EINTR;
        task_wait(&tf->chan, tf->next_tick ? tf->next_tick : 0);
    }
}

static bool timerfd_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    struct timerfd *tf = (struct timerfd *)o;
    if ((events & ABI_POLLIN) &&
        (tf->expirations || (tf->next_tick && tf->next_tick <= pit_get_ticks())))
        *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static int timerfd_close(struct kobject *o) {
    struct timerfd *tf = (struct timerfd *)o;
    uint64_t tf_flags = 0;
    spin_lock_irqsave(&timerfd_lock, &tf_flags);
    timerfd_list_del(tf);
    spin_unlock_irqrestore(&timerfd_lock, tf_flags);
    return 0;
}

static const struct kobject_ops timerfd_ops = {
    .read_at = timerfd_read_at,
    .wait_readable = timerfd_wait_readable,
    .close = timerfd_close,
    .poll_ready = timerfd_poll_read,
};

void timerfd_tick(uint64_t now) {
    bool any = false;

    uint64_t tf_flags = 0;
    spin_lock_irqsave(&timerfd_lock, &tf_flags);
    for (struct timerfd *tf = timerfd_active; tf; ) {
        struct timerfd *nxt = tf->tnext;
        if (tf->next_tick && now >= tf->next_tick) {
            uint64_t hit = 1;
            if (tf->period_ticks) {
                hit += (now - tf->next_tick) / tf->period_ticks;
                tf->next_tick += hit * tf->period_ticks;
                if (tf->next_tick <= now) tf->next_tick = now + tf->period_ticks;
            } else {
                tf->next_tick = 0;
                timerfd_list_del(tf);
            }
            tf->expirations += hit;
            task_wake_chan(&tf->chan);
            any = true;
        }
        tf = nxt;
    }
    spin_unlock_irqrestore(&timerfd_lock, tf_flags);

    if (any) poll_notify();
}

int timerfd_create(int clockid, uint32_t flags, uint16_t *out_flags) {
    if (clockid != 0 && clockid != 1) return FD_EINVAL;

    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct timerfd *tf = (struct timerfd *)kmalloc(sizeof(struct timerfd));
    if (!tf) return FD_ENOMEM;
    memset(tf, 0, sizeof(struct timerfd));

    kobject_init(&tf->obj, KOBJ_TIMERFD,
                 FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, &timerfd_ops);
    tf->obj.user_bufs = true;

    uint16_t fflags = 0;
    if (flags & ABI_TFD_NONBLOCK) fflags |= FD_FLAG_NONBLOCK;
    if (flags & ABI_TFD_CLOEXEC) fflags |= FD_FLAG_CLOEXEC;
    if (out_flags) *out_flags = fflags;

    int fd = fd_alloc(t, &tf->obj,
                      FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, fflags);
    kobject_put(&tf->obj);
    return fd;
}

int timerfd_settime(int fd, unsigned flags, const void *newv, void *oldv) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct kobject *o = fd_get_checked(t, fd, KOBJ_TIMERFD, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    struct timerfd *tf = (struct timerfd *)o;

    struct sys_itimerspec nv, ov;
    if (newv && !copyin(&nv, newv, sizeof nv)) { kobject_put(o); return FD_EFAULT; }

    uint64_t now = pit_get_ticks();
    uint64_t tf_flags = 0;
    spin_lock_irqsave(&timerfd_lock, &tf_flags);

    uint64_t period_ms = pit_ticks_to_ms(tf->period_ticks);
    ov.it_interval.sec = (int64_t)(period_ms / 1000);
    ov.it_interval.nsec = (int64_t)(ms_to_ns(period_ms % 1000));
    if (tf->next_tick && tf->next_tick > now) {
        uint64_t rem = pit_ticks_to_ms(tf->next_tick - now);
        ov.it_value.sec = (int64_t)(rem / 1000);
        ov.it_value.nsec = (int64_t)(ms_to_ns(rem % 1000));
    } else {
        ov.it_value.sec = 0;
        ov.it_value.nsec = 0;
    }

    if (oldv && !copyout(oldv, &ov, sizeof ov)) {
        spin_unlock_irqrestore(&timerfd_lock, tf_flags);
        kobject_put(o);
        return FD_EFAULT;
    }

    bool was_armed = tf->next_tick != 0;
    tf->period_ticks = 0;
    tf->next_tick = 0;

    if (newv && (nv.it_value.sec != 0 || nv.it_value.nsec != 0)) {
        uint64_t vms = ns_to_ms((uint64_t)nv.it_value.sec * 1000000000ULL + (uint64_t)nv.it_value.nsec);
        if (vms == 0) vms = 1;
        uint64_t vticks = pit_ms_to_ticks(vms);
        if (vticks == 0) vticks = 1;
        tf->next_tick = (flags & ABI_TFD_TIMER_ABSTIME) ? vticks : now + vticks;
        if (nv.it_interval.sec != 0 || nv.it_interval.nsec != 0)
            tf->period_ticks = pit_ms_to_ticks(ns_to_ms(
                (uint64_t)nv.it_interval.sec * 1000000000ULL +
                (uint64_t)nv.it_interval.nsec));
    }

    bool armed = tf->next_tick != 0;
    if (armed && !was_armed) timerfd_list_add(tf);
    else if (!armed && was_armed) timerfd_list_del(tf);

    spin_unlock_irqrestore(&timerfd_lock, tf_flags);
    kobject_put(o);
    return 0;
}

int timerfd_gettime(int fd, void *val) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct kobject *o = fd_get_checked(t, fd, KOBJ_TIMERFD, 0);
    if (!o) return FD_EBADF;
    struct timerfd *tf = (struct timerfd *)o;

    struct sys_itimerspec v;
    uint64_t now = pit_get_ticks();

    uint64_t tf_flags = 0;
    spin_lock_irqsave(&timerfd_lock, &tf_flags);
    uint64_t period_ms = pit_ticks_to_ms(tf->period_ticks);
    v.it_interval.sec = (int64_t)(period_ms / 1000);
    v.it_interval.nsec = (int64_t)(ms_to_ns(period_ms % 1000));
    if (tf->next_tick && tf->next_tick > now) {
        uint64_t rem = pit_ticks_to_ms(tf->next_tick - now);
        v.it_value.sec = (int64_t)(rem / 1000);
        v.it_value.nsec = (int64_t)(ms_to_ns(rem % 1000));
    } else {
        v.it_value.sec = 0;
        v.it_value.nsec = 0;
    }
    spin_unlock_irqrestore(&timerfd_lock, tf_flags);

    int r = copyout(val, &v, sizeof v) ? 0 : FD_EFAULT;
    kobject_put(o);
    return r;
}
