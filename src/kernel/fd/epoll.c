#include "kernel/lib/string.h"
#include "kernel/fd/fd.h"
#include "kernel/fd/epoll.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"
#include "arch/x86_64/pit/pit.h"
#include "abi/abi.h"

struct epoll_item {
    struct epoll_item *next;
    int fd;
    struct kobject *target;
    uint16_t events;
    uint64_t data;
};

struct epoll {
    struct kobject obj;
    struct epoll_item *items;
    spinlock_t lock;
};

static int epoll_close(struct kobject *o) {
    struct epoll *ep = (struct epoll *)o;
    struct epoll_item *it = ep->items;
    while (it) {
        struct epoll_item *nxt = it->next;
        kobject_put(it->target);
        kfree(it);
        it = nxt;
    }
    ep->items = NULL;
    return 0;
}

static const struct kobject_ops epoll_ops = {
    .close = epoll_close,
};

int epoll_create1(uint32_t flags, uint16_t *out_flags) {
    (void)flags;
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct epoll *ep = (struct epoll *)kmalloc(sizeof(struct epoll));
    if (!ep) return FD_ENOMEM;
    memset(ep, 0, sizeof(struct epoll));

    kobject_init(&ep->obj, KOBJ_EPOLL,
                 FD_RIGHT_READ | FD_RIGHT_DUP, &epoll_ops);
    (void)ep->lock;

    uint16_t fflags = 0;
    if (flags & ABI_EPOLL_CLOEXEC) fflags |= FD_FLAG_CLOEXEC;
    if (out_flags) *out_flags = fflags;

    int fd = fd_alloc(t, &ep->obj, FD_RIGHT_READ | FD_RIGHT_DUP, fflags);
    kobject_put(&ep->obj);
    if (fd < 0) kobject_free(&ep->obj);
    return fd;
}

int epoll_ctl(int epfd, int op, int fd, const void *event) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct kobject *epo = fd_get_checked(t, epfd, KOBJ_EPOLL, FD_RIGHT_READ);
    if (!epo) return FD_EBADF;
    struct epoll *ep = (struct epoll *)epo;

    struct sys_epoll_event ev;
    uint64_t data = 0;
    uint16_t events = 0;
    if (op == ABI_EPOLL_CTL_ADD || op == ABI_EPOLL_CTL_MOD) {
        if (!event) { kobject_put(epo); return FD_EINVAL; }
        if (!copyin(&ev, event, sizeof ev)) { kobject_put(epo); return FD_EFAULT; }
        data = ev.data;
        events = (uint16_t)(ev.events & 0xFFFF);
    }

    int r = 0;
    spin_lock(&ep->lock);

    if (op == ABI_EPOLL_CTL_ADD) {
        struct epoll_item *it = ep->items;
        while (it) {
            if (it->fd == fd) break;
            it = it->next;
        }
        if (it) {
            r = FD_EEXIST;
        } else {
            struct kobject *to = fd_get_checked(t, fd, KOBJ_TYPE_ANY, 0);
            if (!to) {
                r = FD_EBADF;
            } else {
                struct epoll_item *ni = (struct epoll_item *)kmalloc(sizeof(struct epoll_item));
                if (!ni) {
                    kobject_put(to);
                    r = FD_ENOMEM;
                } else {
                    ni->fd = fd;
                    ni->target = to;
                    ni->events = events;
                    ni->data = data;
                    ni->next = ep->items;
                    ep->items = ni;
                }
            }
        }
    } else if (op == ABI_EPOLL_CTL_DEL) {
        struct epoll_item **pp = &ep->items;
        struct epoll_item *it = NULL;
        while (*pp) {
            if ((*pp)->fd == fd) { it = *pp; *pp = it->next; break; }
            pp = &(*pp)->next;
        }
        if (!it) {
            r = FD_ENOENT;
        } else {
            kobject_put(it->target);
            kfree(it);
        }
    } else if (op == ABI_EPOLL_CTL_MOD) {
        struct epoll_item *it = ep->items;
        while (it) {
            if (it->fd == fd) break;
            it = it->next;
        }
        if (!it) {
            r = FD_ENOENT;
        } else {
            it->events = events;
            it->data = data;
        }
    } else {
        r = FD_EINVAL;
    }

    spin_unlock(&ep->lock);
    kobject_put(epo);
    return r;
}

int epoll_wait(int epfd, void *events, int maxevents, int timeout_ms) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;
    if (maxevents <= 0) return FD_EINVAL;

    struct kobject *epo = fd_get_checked(t, epfd, KOBJ_EPOLL, FD_RIGHT_READ);
    if (!epo) return FD_EBADF;
    struct epoll *ep = (struct epoll *)epo;

    uint64_t deadline = 0;
    if (timeout_ms > 0) deadline = pit_get_ticks() + pit_ms_to_ticks((uint64_t)timeout_ms);

    for (;;) {
        int ready = 0;
        struct sys_epoll_event *out = (struct sys_epoll_event *)events;

        spin_lock(&ep->lock);
        for (struct epoll_item *it = ep->items; it && ready < maxevents; it = it->next) {
            uint16_t revents = 0;
            if (it->target->ops && it->target->ops->poll_ready)
                it->target->ops->poll_ready(it->target, it->events, &revents);
            else
                revents = it->events;

            uint16_t ev = revents & (uint16_t)(it->events | ABI_EPOLLERR | ABI_EPOLLHUP);
            if (ev) {
                out[ready].events = ev;
                out[ready].data = it->data;
                ready++;
            }
        }
        spin_unlock(&ep->lock);

        if (ready) {
            kobject_put(epo);
            return ready;
        }
        if (timeout_ms == 0) {
            kobject_put(epo);
            return 0;
        }
        if (deadline && pit_get_ticks() >= deadline) {
            kobject_put(epo);
            return 0;
        }
        if (task_sig_pending()) {
            kobject_put(epo);
            return FD_EINTR;
        }
        task_wait(poll_chan(), deadline);
    }
}

void epoll_remove_fd(struct task *t, int fd) {
    if (!t) return;
    struct fd_table *ft = &t->fds;
    for (uint32_t i = 0; i < ft->capacity; i++) {
        struct fd_entry *e = (i < FD_INLINE) ? &ft->inl[i] : &ft->heap[i - FD_INLINE];
        if (!e->obj || e->obj->type != KOBJ_EPOLL) continue;
        struct epoll *ep = (struct epoll *)e->obj;
        spin_lock(&ep->lock);
        struct epoll_item **pp = &ep->items;
        while (*pp) {
            if ((*pp)->fd == fd) {
                struct epoll_item *it = *pp;
                *pp = it->next;
                kobject_put(it->target);
                kfree(it);
            } else {
                pp = &(*pp)->next;
            }
        }
        spin_unlock(&ep->lock);
    }
}
