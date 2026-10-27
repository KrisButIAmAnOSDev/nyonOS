#include "kernel/fd/fd.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"
#include "drivers/keyboard/keyboard.h"
#include "kernel/tty/tty.h"

static struct console the_ttyin;
static struct console the_ttyout;
static struct console the_ttyerr;

void kobject_init(struct kobject *o, uint32_t type, uint32_t max_rights,
                  const struct kobject_ops *ops) {
    o->magic = KOBJ_MAGIC;
    o->type = type;
    o->refcount = 1;
    o->max_rights = max_rights;
    o->user_bufs = false;
    o->ops = ops;
}

bool kobject_valid(const struct kobject *o) {
    return o && o->magic == KOBJ_MAGIC;
}

void kobject_get(struct kobject *o) {
    if (!kobject_valid(o)) return;
    uint32_t old = __atomic_fetch_add(&o->refcount, 1, __ATOMIC_RELAXED);
    if (old >= KOBJ_REF_SATURATE) {
        __atomic_fetch_sub(&o->refcount, 1, __ATOMIC_RELAXED);
    }
}

void kobject_put(struct kobject *o) {
    if (!kobject_valid(o)) return;

    uint32_t old = __atomic_fetch_sub(&o->refcount, 1, __ATOMIC_ACQ_REL);
    if (old == 0) {
        __atomic_fetch_add(&o->refcount, 1, __ATOMIC_RELAXED);
        panic_assert("kobject_put refcount underflow");
    }
    if (old >= KOBJ_REF_SATURATE) {
        __atomic_fetch_add(&o->refcount, 1, __ATOMIC_RELAXED);
        return;
    }
    if (old != 1) return;

    if (o->ops && o->ops->close) o->ops->close(o);

    if (o == &the_ttyin.obj || o == &the_ttyout.obj || o == &the_ttyerr.obj) return;

    o->magic = KOBJ_MAGIC_FREE;
    o->type = 0;
    kfree(o);
}

static struct fd_entry *slot(struct fd_table *ft, uint32_t i) {
    return i < FD_INLINE ? &ft->inl[i] : &ft->heap[i - FD_INLINE];
}

static void mark_free(struct fd_table *ft, uint32_t i) {
    ft->freebits[i / 64] |= (1ULL << (i % 64));
}

static void mark_used(struct fd_table *ft, uint32_t i) {
    ft->freebits[i / 64] &= ~(1ULL << (i % 64));
}

void fd_table_clone(struct task *dst, struct task *src) {
    struct fd_table *s = &src->fds;
    struct fd_table *d = &dst->fds;

    uint32_t cap = s->capacity;
    if (cap < FD_INLINE) cap = FD_INLINE;

    for (uint32_t w = 0; w < 4; w++) d->freebits[w] = s->freebits[w];
    d->capacity = cap;
    d->grown = s->grown;
    d->heap = NULL;

    for (uint32_t i = 0; i < cap; i++) {
        struct fd_entry *se = (i < FD_INLINE) ? &s->inl[i] : &s->heap[i - FD_INLINE];
        struct fd_entry tmp = { .obj = NULL, .rights = 0, .flags = 0 };
        if (se->obj) {
            tmp.obj = se->obj;
            tmp.rights = se->rights;
            tmp.flags = se->flags;
            kobject_get(se->obj);
        }
        if (i < FD_INLINE) {
            d->inl[i] = tmp;
        } else {
            if (!d->heap) {
                d->heap = kmalloc((size_t)(cap - FD_INLINE) * sizeof(struct fd_entry));
                if (!d->heap) {
                    for (uint32_t k = 0; k < i; k++) {
                        if (d->inl[k].obj) kobject_put(d->inl[k].obj);
                    }
                    for (uint32_t k = FD_INLINE; k < cap; k++) mark_free(d, k);
                    d->capacity = FD_INLINE;
                    d->grown = false;
                    return;
                }
            }
            d->heap[i - FD_INLINE] = tmp;
        }
    }
}

void fd_table_init(struct task *t) {
    struct fd_table *ft = &t->fds;

    for (uint32_t i = 0; i < FD_INLINE; i++) {
        ft->inl[i].obj = NULL;
        ft->inl[i].rights = 0;
        ft->inl[i].flags = 0;
    }
    for (uint32_t w = 0; w < 4; w++) ft->freebits[w] = 0;
    ft->freebits[0] = (1ULL << FD_INLINE) - 1;

    ft->heap = NULL;
    ft->capacity = FD_INLINE;
    ft->grown = false;
}

void fd_table_clear(struct task *t) {
    struct fd_table *ft = &t->fds;
    struct kobject *batch[32];

    for (uint32_t base = 0; base < FD_MAX_TOTAL; base += 32) {
        uint32_t n = 0;

        preempt_disable();
        for (uint32_t fd = base; fd < ft->capacity && n < 32; fd++) {
            struct fd_entry *e = slot(ft, fd);
            struct kobject *o = e->obj;
            if (!o) continue;
            e->obj = NULL;
            e->rights = 0;
            e->flags = 0;
            mark_free(ft, fd);
            batch[n++] = o;
        }
        preempt_enable();

        for (uint32_t i = 0; i < n; i++) kobject_put(batch[i]);
    }

    if (ft->heap) {
        kfree(ft->heap);
        ft->heap = NULL;
    }
    ft->capacity = FD_INLINE;
    ft->grown = false;
    for (uint32_t w = 0; w < 4; w++) ft->freebits[w] = 0;
    ft->freebits[0] = (1ULL << FD_INLINE) - 1;
}

static bool fd_grow(struct fd_table *ft) {
    if (ft->grown) return true;

    uint32_t n = FD_MAX_TOTAL - FD_INLINE;
    struct fd_entry *big = kmalloc(n * sizeof(struct fd_entry));
    if (!big) return false;

    for (uint32_t i = 0; i < n; i++) {
        big[i].obj = NULL;
        big[i].rights = 0;
        big[i].flags = 0;
    }

    ft->heap = big;
    ft->capacity = FD_MAX_TOTAL;
    ft->grown = true;

    for (uint32_t w = 0; w < 4; w++) ft->freebits[w] = 0;
    ft->freebits[0] = ~((1ULL << FD_INLINE) - 1);
    for (uint32_t i = 0; i < FD_INLINE; i++) {
        if (!ft->inl[i].obj) ft->freebits[0] |= 1ULL << i;
    }
    return true;
}

int fd_alloc(struct task *t, struct kobject *obj, uint32_t rights, uint32_t flags) {
    if (!kobject_valid(obj)) return FD_EBADTYPE;

    preempt_disable();

    struct fd_table *ft = &t->fds;
    uint32_t eff = rights & obj->max_rights;
    int ret = FD_EMFILE;

    for (uint32_t w = 0; w * 64 < ft->capacity; w++) {
        uint64_t word = ft->freebits[w];
        if (!word) continue;

        uint32_t bit = (uint32_t)__builtin_ctzll(word);
        uint32_t idx = w * 64 + bit;
        if (idx >= ft->capacity) break;

        if (idx >= FD_INLINE && !ft->grown) {
            if (!fd_grow(ft)) break;
            w = (uint32_t)-1;
            continue;
        }

        mark_used(ft, idx);
        struct fd_entry *e = slot(ft, idx);
        e->obj = obj;
        e->rights = (uint16_t)eff;
        e->flags = (uint16_t)flags;
        kobject_get(obj);
        ret = (int)idx;
        break;
    }

    bool needs_grow = (ret == FD_EMFILE) && !ft->grown;

    preempt_enable();

    if (needs_grow) {
        preempt_disable();
        bool ok = fd_grow(ft);
        preempt_enable();
        if (ok) return fd_alloc(t, obj, rights, flags);
    }

    return ret;
}

int fd_close(struct task *t, int fd) {
    preempt_disable();

    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;

    struct kobject *o = NULL;

    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj) {
            o = e->obj;
            e->obj = NULL;
            e->rights = 0;
            e->flags = 0;
            mark_free(ft, idx);
        }
    }

    preempt_enable();

    if (!o) return FD_EBADF;
    kobject_put(o);
    return 0;
}

int fd_dup(struct task *t, int fd, uint32_t rights, uint16_t flags) {
    preempt_disable();

    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;

    struct kobject *o = NULL;
    uint16_t keep = 0;
    uint16_t fl = 0;
    bool no_dup = false;

    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj) {
            if (!(e->rights & FD_RIGHT_DUP)) {
                no_dup = true;
            } else {
                o = e->obj;
                keep = (uint16_t)(e->rights & rights);
                fl = e->flags;
            }
        }
    }

    preempt_enable();

    if (no_dup) return FD_EACCES;
    if (!o) return FD_EBADF;
    return fd_alloc(t, o, keep, (uint16_t)((fl & ~(uint16_t)FD_FLAG_CLOEXEC) | flags));
}

void fd_close_cloexec(struct task *t) {
    struct fd_table *ft = &t->fds;
    for (uint32_t i = 0; i < ft->capacity; i++) {
        struct fd_entry *e = slot(ft, i);
        if (e->obj && (e->flags & FD_FLAG_CLOEXEC)) fd_close(t, (int)i);
    }
}

int fd_truncate(struct task *t, int fd, uint64_t len) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_FILE, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    struct file *f = (struct file *)o;
    int r = fs_node_resize(&f->node, (uint32_t)len);
    kobject_put(o);
    return r;
}

int fd_sync(struct task *t, int fd) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_FILE, 0);
    if (!o) return FD_EBADF;
    kobject_put(o);
    return fs_sync() ? 0 : ABI_EIO;
}

int fd_utimens(struct task *t, int fd, int64_t mtime) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_FILE, 0);
    if (!o) return FD_EBADF;
    struct file *f = (struct file *)o;
    int r = fs_node_utimens(&f->node, mtime);
    kobject_put(o);
    return r;
}

int fd_get_rights(struct task *t, int fd, uint16_t *rights) {
    preempt_disable();
    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj) { *rights = e->rights; r = 0; }
    }
    preempt_enable();
    return r;
}

int fd_set_cloexec(struct task *t, int fd, bool on) {
    preempt_disable();
    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj) {
            e->flags = on ? (e->flags | FD_FLAG_CLOEXEC) : (e->flags & ~FD_FLAG_CLOEXEC);
            r = 0;
        }
    }
    preempt_enable();
    return r;
}

int fd_get_cloexec(struct task *t, int fd, bool *on) {
    uint16_t f = 0;
    if (fd_get_status(t, fd, &f)) return FD_EBADF;
    *on = (f & FD_FLAG_CLOEXEC) != 0;
    return 0;
}

int fd_get_status(struct task *t, int fd, uint16_t *flags) {
    preempt_disable();
    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj) { *flags = e->flags; r = 0; }
    }
    preempt_enable();
    return r;
}

int fd_set_status(struct task *t, int fd, uint16_t flags) {
    preempt_disable();
    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj) {
            e->flags = (uint16_t)((e->flags & ~FD_FLAG_NONBLOCK) | (flags & FD_FLAG_NONBLOCK));
            r = 0;
        }
    }
    preempt_enable();
    return r;
}

int fd_slot_in_use(struct task *t, int idx) {
    struct fd_table *ft = &t->fds;
    if (idx < 0 || (uint32_t)idx >= ft->capacity) return 0;
    return slot(ft, (uint32_t)idx)->obj != NULL;
}

int fd_dup2(struct task *t, int oldfd, int newfd, uint32_t flags) {
    preempt_disable();

    struct fd_table *ft = &t->fds;
    uint32_t oidx = (uint32_t)oldfd & FD_INDEX_MASK;
    uint32_t nidx = (uint32_t)newfd & FD_INDEX_MASK;

    struct kobject *o = NULL;
    uint16_t old_rights = 0;
    bool found = false;

    if (oldfd >= 0 && oidx < ft->capacity) {
        struct fd_entry *e = slot(ft, oidx);
        if (e->obj) {
            o = e->obj;
            old_rights = e->rights;
            found = true;
        }
    }

    if (!found || newfd < 0 || newfd >= FD_MAX_TOTAL) {
        preempt_enable();
        return FD_EBADF;
    }

    if (oldfd == newfd) {
        preempt_enable();
        return newfd;
    }

    if (newfd >= (int)ft->capacity && !fd_grow(ft)) {
        preempt_enable();
        return FD_EMFILE;
    }

    struct fd_entry *target = slot(ft, nidx);
    if (target->obj) {
        struct kobject *old = target->obj;
        target->obj = NULL;
        target->rights = 0;
        target->flags = 0;
        mark_free(ft, nidx);
        kobject_put(old);
    }

    mark_used(ft, nidx);
    kobject_get(o);
    target->obj = o;
    target->rights = old_rights;
    target->flags = (uint16_t)flags;

    preempt_enable();
    return (int)nidx;
}

int fd_dup_min(struct task *t, int oldfd, uint32_t min) {
    uint16_t keep = FD_RIGHTS_ALL;
    if (fd_get_rights(t, oldfd, &keep)) return FD_EBADF;
    if (min >= FD_MAX_TOTAL) return FD_EINVAL;

    for (uint32_t i = min; i < FD_MAX_TOTAL; i++) {
        if (fd_slot_in_use(t, (int)i)) continue;
        return fd_dup2(t, oldfd, (int)i, 0);
    }
    return FD_EMFILE;
}

struct kobject *fd_get_checked(struct task *t, int fd, uint32_t type, uint32_t needed) {
    preempt_disable();

    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;

    struct kobject *o = NULL;

    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj &&
            kobject_valid(e->obj) &&
            (type == KOBJ_TYPE_ANY || e->obj->type == type) &&
            (e->rights & needed) == needed) {
            kobject_get(e->obj);
            o = e->obj;
        }
    }

    preempt_enable();
    return o;
}

void fd_put(struct kobject *o) {
    kobject_put(o);
}

int64_t fd_read(struct task *t, int fd, void *ubuf, uint32_t len) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, FD_RIGHT_READ);
    if (!o) return FD_EBADF;
    if (!o->ops || !o->ops->read_at) { kobject_put(o); return FD_EBADF; }

    if (o->type != KOBJ_FILE) {
        uint16_t fl = 0;
        fd_get_status(t, fd, &fl);
        int64_t r;
        for (;;) {
            r = o->ops->read_at(o, ubuf, len, 0);
            if (r != FD_EAGAIN || (fl & FD_FLAG_NONBLOCK) || !o->ops->wait_readable) break;
            int w = o->ops->wait_readable(o);
            if (w < 0) { r = w; break; }
        }
        kobject_put(o);
        return r;
    }

    struct file *f = (struct file *)o;
    int64_t r = o->ops->read_at(o, ubuf, len, f->pos);
    if (r > 0) __atomic_fetch_add(&f->pos, (uint64_t)r, __ATOMIC_RELAXED);

    kobject_put(o);
    return r;
}

int64_t fd_write(struct task *t, int fd, const void *ubuf, uint32_t len) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    if (!o->ops || !o->ops->write_at) { kobject_put(o); return FD_EBADF; }

    uint16_t fl = 0;
    fd_get_status(t, fd, &fl);

    struct file *f = NULL;
    uint64_t pos = 0;
    if (o->type == KOBJ_FILE) {
        f = (struct file *)o;
        if (f->append) f->pos = f->node.size;
        pos = f->pos;
    }

    int64_t total = 0;
    for (;;) {
        int64_t r = o->ops->write_at(o, (const char *)ubuf + total,
                                     len - (uint32_t)total, pos + total);
        if (r < 0) {
            if (r == FD_EAGAIN && !(fl & FD_FLAG_NONBLOCK) && o->ops->wait_writable) {
                int w = o->ops->wait_writable(o);
                if (w < 0) { if (!total) total = w; break; }
                continue;
            }
            if (!total) total = r;
            break;
        }
        total += r;
        if (!r) break;
    }

    if (f) __atomic_fetch_add(&f->pos, (uint64_t)total, __ATOMIC_RELAXED);

    kobject_put(o);
    return total;
}

static char poll_wait_chan;

void poll_notify(void) {
    task_wake_chan(&poll_wait_chan);
}

void *poll_chan(void) {
    return &poll_wait_chan;
}

bool fd_uses_user_bufs(struct task *t, int fd) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, 0);
    if (!o) return false;
    bool r = o->user_bufs;
    kobject_put(o);
    return r;
}

bool fd_poll_ready(struct task *t, int fd, uint16_t events, uint16_t *revents) {
    *revents = 0;
    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, 0);
    if (!o) return false;
    bool r = false;
    if (o->ops && o->ops->poll_ready) r = o->ops->poll_ready(o, events, revents);
    kobject_put(o);
    return r;
}

int64_t fd_lseek(struct task *t, int fd, int64_t off, int whence) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, 0);
    if (!o) return FD_EBADF;

    if (o->type != KOBJ_FILE) {
        kobject_put(o);
        return FD_ESPIPE;
    }

    struct file *f = (struct file *)o;
    int64_t r = -1;

    if (o->ops && o->ops->lseek) r = o->ops->lseek(o, off, whence);
    if (r >= 0) f->pos = (uint64_t)r;

    kobject_put(o);
    return r;
}

static int64_t file_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    struct file *f = (struct file *)o;
    if (off >= f->node.size) return 0;

    uint64_t want = f->node.size - off;
    if (want > len) want = len;

    if (!fs_node_read(&f->node, (uint32_t)off, ubuf, (uint32_t)want)) return FD_EIO;
    return (int64_t)want;
}

static int64_t file_write_at(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off) {
    struct file *f = (struct file *)o;
    if (!f->writable) return FD_EACCES;
    int r = fs_node_write(&f->node, (uint32_t)off, ubuf, len);
    if (r < 0) return r;
    return r;
}

static int64_t file_lseek(struct kobject *o, int64_t off, int whence) {
    struct file *f = (struct file *)o;
    int64_t base;

    if (whence == FD_SEEK_SET) base = 0;
    else if (whence == FD_SEEK_CUR) base = (int64_t)f->pos;
    else if (whence == FD_SEEK_END) base = (int64_t)f->node.size;
    else return FD_EINVAL;

    int64_t np = base + off;
    if (np < 0) return FD_EINVAL;
    return np;
}

static int64_t console_write_at(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    const struct console *c = (const struct console *)o;
    if (c->dest == PRINT_NONE) return (int64_t)len;
    const char *s = (const char *)ubuf;
    kprintf_output_lock();
    for (uint32_t i = 0; i < len; i++) kprintchar(s[i], c->dest);
    kprintf_output_unlock();
    return (int64_t)len;
}

static bool file_poll_ready(struct kobject *o, uint16_t events, uint16_t *revents) {
    (void)o;
    if (events & ABI_POLLIN) *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static const struct kobject_ops file_ops = {
    .read_at = file_read_at,
    .write_at = file_write_at,
    .lseek = file_lseek,
    .close = NULL,
    .poll_ready = file_poll_ready,
};

int fd_console_dest(int fd, struct task *t, uint32_t *dest) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_CONSOLE, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    *dest = ((struct console *)o)->dest;
    kobject_put(o);
    return 0;
}

int fd_console_set_dest(int fd, struct task *t, uint32_t dest) {
    if (dest > PRINT_NONE) return FD_EINVAL;
    struct kobject *o = fd_get_checked(t, fd, KOBJ_CONSOLE, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    ((struct console *)o)->dest = dest;
    kobject_put(o);
    return 0;
}

int fd_tty_get_fg(int fd, struct task *t, uint32_t *pid) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_CONSOLE, 0);
    if (!o) return FD_EBADF;
    kobject_put(o);
    *pid = keyboard_owner();
    return 0;
}

int fd_tty_set_fg(int fd, struct task *t, uint32_t pid) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_CONSOLE, 0);
    if (!o) return FD_EBADF;
    kobject_put(o);

    int r = 0;
    preempt_disable();
    uint32_t owner = keyboard_owner();
    if (owner != 0 && owner != t->pid) r = FD_EPERM;
    else if (pid != 0 && !task_find_pid(pid)) r = FD_ESRCH;
    else keyboard_set_owner(pid);
    preempt_enable();
    task_wake_chan(keyboard_wait_chan());
    return r;
}

void fd_tty_task_gone(uint32_t pid) {
    preempt_disable();
    if (pid != 0 && keyboard_owner() == pid) keyboard_set_owner(0);
    preempt_enable();
    task_wake_chan(keyboard_wait_chan());
}

static int64_t console_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)o; (void)off;
    if (!ubuf || !len) return 0;

    struct task *t = task_current();
    uint32_t me = t ? t->pid : 0;

    preempt_disable();
    uint32_t owner = keyboard_owner();
    if (owner == 0 && me != 0) {
        keyboard_set_owner(me);
        owner = me;
    }
    preempt_enable();
    if (owner != me) return FD_EIO;

    char *s = (char *)ubuf;
    uint32_t i = 0;

    if (!tty_canonical()) {
        preempt_disable();
        while (i < len) {
            char c;
            if (!keyboard_try_pop(&c)) break;
            s[i++] = c;
        }
        preempt_enable();
        if (i == 0) return FD_EAGAIN;
        return (int64_t)i;
    }

    if (!tty_input_ready()) return FD_EAGAIN;

    char eof = tty_eof_char();

    preempt_disable();
    while (i < len) {
        char c;
        if (!keyboard_try_pop(&c)) break;
        if (eof && c == eof) {
            if (i == 0) {
                preempt_enable();
                return 0;
            }
            break;
        }
        s[i++] = c;
        if (c == '\n') break;
    }
    preempt_enable();

    if (i == 0) return FD_EAGAIN;
    return (int64_t)i;
}

static int console_wait_readable(struct kobject *o) {
    (void)o;
    struct task *t = task_current();
    uint32_t me = t ? t->pid : 0;

    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");

    int r;
    for (;;) {
        if (keyboard_owner() != me) { r = FD_EIO; break; }
        if (tty_input_ready()) { r = 0; break; }
        task_wait(keyboard_wait_chan(), 0);
    }

    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return r;
}

static bool console_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    (void)o;
    if ((events & ABI_POLLIN) && tty_input_ready()) *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static bool console_poll_write(struct kobject *o, uint16_t events, uint16_t *revents) {
    (void)o;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static const struct kobject_ops tty_in_ops = {
    .wait_readable = console_wait_readable,
    .read_at = console_read_at,
    .write_at = NULL,
    .lseek = NULL,
    .close = NULL,
    .poll_ready = console_poll_read,
};

static const struct kobject_ops tty_out_ops = {
    .read_at = NULL,
    .write_at = console_write_at,
    .lseek = NULL,
    .close = NULL,
    .poll_ready = console_poll_write,
};

static const struct kobject_ops tty_err_ops = {
    .read_at = NULL,
    .write_at = console_write_at,
    .lseek = NULL,
    .close = NULL,
    .poll_ready = console_poll_write,
};

static int64_t input_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    if (!ubuf || len < sizeof(struct sys_input_event)) return 0;
    struct input_file *in = (struct input_file *)o;
    struct sys_input_event ev;
    for (;;) {
        if (!keyboard_event_pop_at(&ev, &in->seq)) return ABI_EAGAIN;
        if (ev.sec == 0) continue;
        break;
    }
    if (!copyout(ubuf, &ev, sizeof ev)) return -ABI_EFAULT;
    return (int64_t)sizeof ev;
}

static int input_wait_readable(struct kobject *o) {
    struct input_file *in = (struct input_file *)o;
    if (keyboard_event_available_at(in->seq)) return 0;
    task_wait(keyboard_input_wait_chan(), 0);
    return 0;
}

static bool input_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    struct input_file *in = (struct input_file *)o;
    if ((events & ABI_POLLIN) && keyboard_event_available_at(in->seq)) *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static int input_close(struct kobject *o) {
    kfree((struct input_file *)o);
    return 0;
}

static const struct kobject_ops input_ops = {
    .wait_readable = input_wait_readable,
    .read_at = input_read_at,
    .write_at = NULL,
    .lseek = NULL,
    .close = input_close,
    .poll_ready = input_poll_read,
};

int fd_open_input(struct task *t, uint32_t oflags) {
    if ((oflags & FD_OPEN_MODE) != FD_OPEN_READ) return FD_EINVAL;
    struct input_file *in = (struct input_file *)kmalloc(sizeof(struct input_file));
    if (!in) return FD_ENOSPC;
    kobject_init(&in->obj, KOBJ_INPUT, FD_RIGHT_READ | FD_RIGHT_DUP, &input_ops);
    in->seq = keyboard_event_seq();
    uint16_t fflags = 0;
    if (oflags & FD_OPEN_NONBLOCK) fflags |= FD_FLAG_NONBLOCK;
    int fd = fd_alloc(t, &in->obj, FD_RIGHT_READ, fflags);
    kobject_put(&in->obj);
    if (fd < 0) kfree(in);
    return fd;
}

bool fd_path_is_input(const char *path) {
    const char *a = "/DEV/INPUT/EVENT0";
    const char *b = "/DEV/INPUT0";
    size_t i = 0;
    while (path[i] && a[i] && (path[i] | 0x20) == (a[i] | 0x20)) i++;
    if (!path[i] && !a[i]) return true;
    i = 0;
    while (path[i] && b[i] && (path[i] | 0x20) == (b[i] | 0x20)) i++;
    return !path[i] && !b[i];
}

int fd_install_stdio(struct task *t) {
    static bool console_ready = false;

    if (!console_ready) {
        kobject_init(&the_ttyin.obj, KOBJ_CONSOLE, FD_RIGHT_READ | FD_RIGHT_DUP, &tty_in_ops);
        the_ttyin.dest = PRINT_BOTH;

        kobject_init(&the_ttyout.obj, KOBJ_CONSOLE, FD_RIGHT_WRITE | FD_RIGHT_DUP, &tty_out_ops);
        the_ttyout.dest = PRINT_BOTH;

        kobject_init(&the_ttyerr.obj, KOBJ_CONSOLE, FD_RIGHT_WRITE | FD_RIGHT_DUP, &tty_err_ops);
        the_ttyerr.dest = PRINT_BOTH;

        console_ready = true;
    }

    int in = fd_alloc(t, &the_ttyin.obj, FD_RIGHT_READ, 0);
    int out = fd_alloc(t, &the_ttyout.obj, FD_RIGHT_WRITE, 0);
    int err = fd_alloc(t, &the_ttyerr.obj, FD_RIGHT_WRITE, 0);

    if (in < 0 || out < 0 || err < 0) {
        if (in >= 0) fd_close(t, in);
        if (out >= 0) fd_close(t, out);
        if (err >= 0) fd_close(t, err);
        return FD_EMFILE;
    }

    return 0;
}

int fd_open(struct task *t, const char *path, uint32_t oflags) {
    uint32_t pl = 0;
    while (path[pl]) pl++;
    if (pl > FD_PATH_MAX) return FD_ENAMETOOLONG;

    if (fd_path_is_input(path)) return fd_open_input(t, oflags);

    fs_node_t node;
    if (!fs_lookup(path, &node)) {
        if (!(oflags & FD_OPEN_CREATE)) return FD_ENOENT;
        int r = fs_create(path, &node);
        if (r) return r;
    } else if (oflags & FD_OPEN_CREATE) {
        int r = fs_create(path, &node);
        if (r != ABI_EEXIST) return r;
        if (!fs_lookup(path, &node)) return FD_ENOENT;
    }
    if (node.is_dir && !(oflags & FD_OPEN_DIR)) return FD_EISDIR;
    if (node.is_dir && (oflags & FD_OPEN_MODE) != FD_OPEN_READ) return FD_EISDIR;

    uint32_t rights = 0;
    uint32_t omode = oflags & FD_OPEN_MODE;
    if (node.is_dir) {
        rights |= FD_RIGHT_GETDENT;
    } else {
        if (omode != FD_OPEN_WRITE) rights |= FD_RIGHT_READ;
        if (omode != FD_OPEN_READ) rights |= FD_RIGHT_WRITE;
    }
    rights |= FD_RIGHT_DUP;

    struct file *f = kmalloc(sizeof(struct file));
    if (!f) return FD_ENOSPC;

    if (oflags & FD_OPEN_TRUNC) {
        if (!node.is_dir) {
            int r = fs_node_truncate(&node);
            if (r) { kfree(f); return r; }
        }
    }

    kobject_init(&f->obj, KOBJ_FILE, FD_RIGHTS_ALL, &file_ops);
    f->node = node;
    f->pos = 0;
    f->writable = (oflags & FD_OPEN_MODE) != FD_OPEN_READ;
    f->append = (oflags & FD_OPEN_APPEND) != 0;
    if (f->append) f->pos = f->node.size;

    uint16_t fflags = 0;
    if (oflags & FD_OPEN_CLOEXEC) fflags |= FD_FLAG_CLOEXEC;
    if (oflags & FD_OPEN_NONBLOCK) fflags |= FD_FLAG_NONBLOCK;

    int fd = fd_alloc(t, &f->obj, rights, fflags);

    kobject_put(&f->obj);

    if (fd < 0) return fd;
    return fd;
}