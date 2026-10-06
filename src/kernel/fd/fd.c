#include "kernel/fd/fd.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "drivers/keyboard/keyboard.h"

static struct console the_ttyin;
static struct console the_ttyout;
static struct console the_ttyerr;

void kobject_init(struct kobject *o, uint32_t type, uint32_t max_rights,
                  const struct kobject_ops *ops) {
    o->magic = KOBJ_MAGIC;
    o->type = type;
    o->refcount = 1;
    o->max_rights = max_rights;
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

void fd_table_init(struct task *t) {
    struct fd_table *ft = &t->fds;

    for (uint32_t i = 0; i < FD_INLINE; i++) {
        ft->inl[i].obj = NULL;
        ft->inl[i].rights = 0;
        ft->inl[i].flags = FD_FLAG_CLOEXEC;
        ft->inl[i].generation = 0;
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
            e->flags = FD_FLAG_CLOEXEC;
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
        big[i].flags = FD_FLAG_CLOEXEC;
        big[i].generation = 0;
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
        e->flags = (uint16_t)(flags | FD_FLAG_CLOEXEC);
        kobject_get(obj);
        ret = (int)(idx | ((uint32_t)e->generation << FD_GEN_SHIFT));
        break;
    }

    preempt_enable();

    if (ret == FD_EMFILE && !ft->grown) {
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
    uint8_t gen = (uint8_t)(((uint32_t)fd >> FD_GEN_SHIFT) & FD_GEN_MASK);

    struct kobject *o = NULL;

    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj && e->generation == gen) {
            o = e->obj;
            e->obj = NULL;
            e->rights = 0;
            e->flags = FD_FLAG_CLOEXEC;
            e->generation = (uint8_t)((e->generation + 1) & FD_GEN_MASK);
            mark_free(ft, idx);
        }
    }

    preempt_enable();

    if (!o) return FD_EBADF;
    kobject_put(o);
    return 0;
}

int fd_dup(struct task *t, int fd, uint32_t rights) {
    preempt_disable();

    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    uint8_t gen = (uint8_t)(((uint32_t)fd >> FD_GEN_SHIFT) & FD_GEN_MASK);

    struct kobject *o = NULL;
    uint16_t keep = 0;
    uint16_t fl = 0;
    bool no_dup = false;

    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj && e->generation == gen) {
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
    return fd_alloc(t, o, keep, fl);
}

int fd_get_rights(struct task *t, int fd, uint16_t *rights) {
    preempt_disable();
    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    uint8_t gen = (uint8_t)(((uint32_t)fd >> FD_GEN_SHIFT) & FD_GEN_MASK);
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj && e->generation == gen) { *rights = e->rights; r = 0; }
    }
    preempt_enable();
    return r;
}

int fd_set_cloexec(struct task *t, int fd, bool on) {
    preempt_disable();
    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    uint8_t gen = (uint8_t)(((uint32_t)fd >> FD_GEN_SHIFT) & FD_GEN_MASK);
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj && e->generation == gen) {
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
    uint8_t gen = (uint8_t)(((uint32_t)fd >> FD_GEN_SHIFT) & FD_GEN_MASK);
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj && e->generation == gen) { *flags = e->flags; r = 0; }
    }
    preempt_enable();
    return r;
}

int fd_set_status(struct task *t, int fd, uint16_t flags) {
    preempt_disable();
    struct fd_table *ft = &t->fds;
    uint32_t idx = (uint32_t)fd & FD_INDEX_MASK;
    uint8_t gen = (uint8_t)(((uint32_t)fd >> FD_GEN_SHIFT) & FD_GEN_MASK);
    int r = FD_EBADF;
    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj && e->generation == gen) {
            e->flags = flags | FD_FLAG_CLOEXEC;
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
    if (oldfd == newfd) return newfd;

    preempt_disable();

    struct fd_table *ft = &t->fds;
    uint32_t oidx = (uint32_t)oldfd & FD_INDEX_MASK;
    uint8_t ogen = (uint8_t)(((uint32_t)oldfd >> FD_GEN_SHIFT) & FD_GEN_MASK);
    uint32_t nidx = (uint32_t)newfd & FD_INDEX_MASK;

    struct kobject *o = NULL;
    uint16_t old_rights = 0;
    uint8_t old_gen = 0;
    bool found = false;

    if (oldfd >= 0 && oidx < ft->capacity) {
        struct fd_entry *e = slot(ft, oidx);
        if (e->obj && e->generation == ogen) {
            o = e->obj;
            old_rights = e->rights;
            old_gen = e->generation;
            found = true;
        }
    }

    if (!found || newfd < 0 || newfd >= FD_MAX_TOTAL) {
        preempt_enable();
        return FD_EBADF;
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
        target->flags = FD_FLAG_CLOEXEC;
        target->generation++;
        mark_free(ft, nidx);
        kobject_put(old);
    }

    mark_used(ft, nidx);
    kobject_get(o);
    target->obj = o;
    target->rights = old_rights;
    target->flags = (uint16_t)(flags | FD_FLAG_CLOEXEC);
    target->generation = old_gen;

    preempt_enable();
    return newfd;
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
    uint8_t gen = (uint8_t)(((uint32_t)fd >> FD_GEN_SHIFT) & FD_GEN_MASK);

    struct kobject *o = NULL;

    if (fd >= 0 && idx < ft->capacity) {
        struct fd_entry *e = slot(ft, idx);
        if (e->obj && e->generation == gen &&
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
        int64_t r = o->ops->read_at(o, ubuf, len, 0);
        kobject_put(o);
        return r;
    }

    struct file *f = (struct file *)o;
    int64_t r = o->ops->read_at(o, ubuf, len, f->pos);
    if (r > 0) f->pos += (uint64_t)r;

    kobject_put(o);
    return r;
}

int64_t fd_write(struct task *t, int fd, const void *ubuf, uint32_t len) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_TYPE_ANY, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    if (!o->ops || !o->ops->write_at) { kobject_put(o); return FD_EBADF; }

    int64_t r = o->ops->write_at(o, ubuf, len, 0);

    kobject_put(o);
    return r;
}

int64_t fd_lseek(struct task *t, int fd, int64_t off, int whence) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_FILE, FD_RIGHT_READ);
    if (!o) return FD_EBADF;

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
    (void)o; (void)ubuf; (void)len; (void)off;
    return FD_EACCES;
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
    if (np > (int64_t)f->node.size) np = (int64_t)f->node.size;
    return np;
}

static int64_t console_write_at(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    const struct console *c = (const struct console *)o;
    const char *s = (const char *)ubuf;
    for (uint32_t i = 0; i < len; i++) kprintchar(s[i], c->dest);
    return (int64_t)len;
}

static const struct kobject_ops file_ops = {
    .read_at = file_read_at,
    .write_at = file_write_at,
    .lseek = file_lseek,
    .close = NULL,
};

int fd_console_dest(int fd, struct task *t, uint32_t *dest) {
    struct kobject *o = fd_get_checked(t, fd, KOBJ_CONSOLE, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    *dest = ((struct console *)o)->dest;
    kobject_put(o);
    return 0;
}

int fd_console_set_dest(int fd, struct task *t, uint32_t dest) {
    if (dest > PRINT_BOTH) return FD_EINVAL;
    struct kobject *o = fd_get_checked(t, fd, KOBJ_CONSOLE, FD_RIGHT_WRITE);
    if (!o) return FD_EBADF;
    ((struct console *)o)->dest = dest;
    kobject_put(o);
    return 0;
}

static int64_t console_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)o; (void)off;
    if (!ubuf || !len) return 0;

    keyboard_claim_stdin();

    char *s = (char *)ubuf;
    uint32_t i = 0;

    preempt_disable();
    while (i < len) {
        char c;
        if (!keyboard_try_pop(&c)) break;
        s[i++] = c;
        if (c == '\n') break;
    }
    preempt_enable();

    if (i == 0) return FD_EAGAIN;
    return (int64_t)i;
}

static const struct kobject_ops tty_in_ops = {
    .read_at = console_read_at,
    .write_at = NULL,
    .lseek = NULL,
    .close = NULL,
};

static const struct kobject_ops tty_out_ops = {
    .read_at = NULL,
    .write_at = console_write_at,
    .lseek = NULL,
    .close = NULL,
};

static const struct kobject_ops tty_err_ops = {
    .read_at = NULL,
    .write_at = console_write_at,
    .lseek = NULL,
    .close = NULL,
};

int fd_install_stdio(struct task *t) {
    static bool console_ready = false;

    if (!console_ready) {
        kobject_init(&the_ttyin.obj, KOBJ_CONSOLE, FD_RIGHT_READ | FD_RIGHT_DUP, &tty_in_ops);
        the_ttyin.dest = PRINT_BOTH;

        kobject_init(&the_ttyout.obj, KOBJ_CONSOLE, FD_RIGHT_WRITE | FD_RIGHT_DUP, &tty_out_ops);
        the_ttyout.dest = PRINT_BOTH;

        kobject_init(&the_ttyerr.obj, KOBJ_CONSOLE, FD_RIGHT_WRITE | FD_RIGHT_DUP, &tty_err_ops);
        the_ttyerr.dest = PRINT_SERIAL;

        console_ready = true;
    }

    int in = fd_alloc(t, &the_ttyin.obj, FD_RIGHT_READ, FD_FLAG_CLOEXEC);
    int out = fd_alloc(t, &the_ttyout.obj, FD_RIGHT_WRITE, FD_FLAG_CLOEXEC);
    int err = fd_alloc(t, &the_ttyerr.obj, FD_RIGHT_WRITE, FD_FLAG_CLOEXEC);

    if (in < 0 || out < 0 || err < 0) {
        if (in >= 0) fd_close(t, in);
        if (out >= 0) fd_close(t, out);
        if (err >= 0) fd_close(t, err);
        return FD_EMFILE;
    }

    return 0;
}

int fd_open(struct task *t, const char *path, uint32_t oflags) {
    fs_node_t node;
    if (!fs_lookup(path, &node)) return FD_ENOENT;
    if (node.is_dir && !(oflags & FD_OPEN_DIR)) return FD_EISDIR;

    uint32_t rights = 0;
    if (node.is_dir) rights |= FD_RIGHT_GETDENT;
    else if (oflags & FD_OPEN_WRITE) rights |= FD_RIGHT_WRITE;
    else rights |= FD_RIGHT_READ;
    rights |= FD_RIGHT_DUP;

    struct file *f = kmalloc(sizeof(struct file));
    if (!f) return FD_ENOSPC;

    kobject_init(&f->obj, KOBJ_FILE, FD_RIGHTS_ALL, &file_ops);
    f->node = node;
    f->pos = 0;
    f->writable = (oflags & FD_OPEN_WRITE) != 0;

    int fd = fd_alloc(t, &f->obj, rights, 0);

    kobject_put(&f->obj);

    if (fd < 0) return fd;
    return fd;
}