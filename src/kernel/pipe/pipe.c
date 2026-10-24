#include "kernel/pipe/pipe.h"
#include "kernel/fd/fd.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"

struct pipe_pair {
    uint8_t *ring;
    uint32_t cap;
    uint32_t count;
    uint32_t head;
    uint32_t read_ends;
    uint32_t write_ends;
    uint32_t refs;
    char chan;
};

struct pipe_end {
    struct kobject obj;
    struct pipe_pair *pair;
    bool is_read;
};

static int64_t pipe_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    if (!ubuf || !len) return 0;

    struct pipe_end *pe = (struct pipe_end *)o;
    struct pipe_pair *p = pe->pair;

    uint32_t n, head, first;
    char *dst = (char *)ubuf;

    preempt_disable();

    if (p->count == 0) {
        bool eof = (p->write_ends == 0);
        preempt_enable();
        return eof ? 0 : FD_EAGAIN;
    }

    n = len < p->count ? len : p->count;
    head = p->head;
    first = p->cap - head;
    if (first > n) first = n;

    if (!copyout(dst, p->ring + head, first)) {
        preempt_enable();
        return FD_EFAULT;
    }
    if (n > first) {
        if (!copyout(dst + first, p->ring, n - first)) {
            preempt_enable();
            return FD_EFAULT;
        }
    }

    p->head = (head + n) % p->cap;
    p->count -= n;

    preempt_enable();

    task_wake_chan(&p->chan);
    poll_notify();

    return (int64_t)n;
}

static int64_t pipe_write_at(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    if (!ubuf || !len) return 0;

    struct pipe_end *pe = (struct pipe_end *)o;
    struct pipe_pair *p = pe->pair;

    uint32_t n, tail, first;
    const char *src = (const char *)ubuf;

    preempt_disable();

    if (p->read_ends == 0) {
        preempt_enable();
        return FD_EPIPE;
    }

    uint32_t space = p->cap - p->count;
    if (space == 0) {
        preempt_enable();
        return FD_EAGAIN;
    }

    if (len <= ABI_PIPE_BUF && space < len) {
        preempt_enable();
        return FD_EAGAIN;
    }

    n = len < space ? len : space;
    tail = (p->head + p->count) % p->cap;
    first = p->cap - tail;
    if (first > n) first = n;

    if (!copyin((void *)(p->ring + tail), src, first)) {
        preempt_enable();
        return FD_EFAULT;
    }
    if (n > first) {
        if (!copyin((void *)p->ring, src + first, n - first)) {
            preempt_enable();
            return FD_EFAULT;
        }
    }

    p->count += n;

    preempt_enable();

    task_wake_chan(&p->chan);
    poll_notify();

    return (int64_t)n;
}

static int pipe_wait_readable(struct kobject *o) {
    struct pipe_end *pe = (struct pipe_end *)o;
    struct pipe_pair *p = pe->pair;

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    for (;;) {
        preempt_disable();
        bool ready = p->count > 0 || p->write_ends == 0;
        preempt_enable();
        if (ready) break;
        task_wait(&p->chan, 0);
    }

    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return 0;
}

static int pipe_wait_writable(struct kobject *o) {
    struct pipe_end *pe = (struct pipe_end *)o;
    struct pipe_pair *p = pe->pair;

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    for (;;) {
        preempt_disable();
        bool ready = (p->cap - p->count) > 0 || p->read_ends == 0;
        preempt_enable();
        if (ready) break;
        task_wait(&p->chan, 0);
    }

    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return 0;
}

static bool pipe_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    struct pipe_end *pe = (struct pipe_end *)o;
    struct pipe_pair *p = pe->pair;

    preempt_disable();
    bool can_read = p->count > 0 || p->write_ends == 0;
    preempt_enable();

    if ((events & ABI_POLLIN) && can_read) *revents |= ABI_POLLIN;
    return *revents != 0;
}

static bool pipe_poll_write(struct kobject *o, uint16_t events, uint16_t *revents) {
    struct pipe_end *pe = (struct pipe_end *)o;
    struct pipe_pair *p = pe->pair;

    preempt_disable();
    bool can_write = (p->cap - p->count) > 0;
    bool broken = p->read_ends == 0;
    preempt_enable();

    if ((events & ABI_POLLOUT) && can_write) *revents |= ABI_POLLOUT;
    if (broken) *revents |= ABI_POLLERR;
    return *revents != 0;
}

static int pipe_close(struct kobject *o) {
    struct pipe_end *pe = (struct pipe_end *)o;
    struct pipe_pair *p = pe->pair;

    preempt_disable();
    if (pe->is_read) p->read_ends--;
    else p->write_ends--;
    uint32_t refs = --p->refs;
    preempt_enable();

    if (refs == 0) {
        kfree(p->ring);
        kfree(p);
        return 0;
    }

    task_wake_chan(&p->chan);
    poll_notify();
    return 0;
}

static const struct kobject_ops pipe_read_ops = {
    .read_at = pipe_read_at,
    .write_at = NULL,
    .lseek = NULL,
    .close = pipe_close,
    .wait_readable = pipe_wait_readable,
    .wait_writable = NULL,
    .poll_ready = pipe_poll_read,
};

static const struct kobject_ops pipe_write_ops = {
    .read_at = NULL,
    .write_at = pipe_write_at,
    .lseek = NULL,
    .close = pipe_close,
    .wait_readable = NULL,
    .wait_writable = pipe_wait_writable,
    .poll_ready = pipe_poll_write,
};

int pipe_create(struct task *t, int fds[2]) {
    if (!t) return -1;

    struct pipe_pair *p = kmalloc(sizeof(struct pipe_pair));
    if (!p) return -1;

    p->ring = kmalloc(PIPE_CAPACITY);
    if (!p->ring) {
        kfree(p);
        return -1;
    }

    p->cap = PIPE_CAPACITY;
    p->count = 0;
    p->head = 0;
    p->read_ends = 1;
    p->write_ends = 1;
    p->refs = 2;

    struct pipe_end *r = kmalloc(sizeof(struct pipe_end));
    struct pipe_end *w = kmalloc(sizeof(struct pipe_end));
    if (!r || !w) {
        if (r) kfree(r);
        if (w) kfree(w);
        kfree(p->ring);
        kfree(p);
        return -1;
    }

    r->pair = p;
    r->is_read = true;
    w->pair = p;
    w->is_read = false;

    kobject_init(&r->obj, KOBJ_PIPE, FD_RIGHT_READ | FD_RIGHT_DUP, &pipe_read_ops);
    kobject_init(&w->obj, KOBJ_PIPE, FD_RIGHT_WRITE | FD_RIGHT_DUP, &pipe_write_ops);
    r->obj.user_bufs = true;
    w->obj.user_bufs = true;

    int rfd = fd_alloc(t, &r->obj, FD_RIGHT_READ | FD_RIGHT_DUP, 0);
    int wfd = -1;
    if (rfd >= 0) wfd = fd_alloc(t, &w->obj, FD_RIGHT_WRITE | FD_RIGHT_DUP, 0);

    kobject_put(&r->obj);
    kobject_put(&w->obj);

    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(t, rfd);
        return -1;
    }

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}
