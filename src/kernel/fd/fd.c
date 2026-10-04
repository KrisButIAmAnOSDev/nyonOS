#include "kernel/fd/fd.h"
#include "kernel/kprintf/kprintf.h"
#include "kernel/panic/panic.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/sync.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"

static struct console the_console;

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

    if (o == &the_console.obj) return;

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
    (void)o; (void)off;
    const char *s = (const char *)ubuf;
    for (uint32_t i = 0; i < len; i++) kprintchar(s[i], PRINT_BOTH);
    return (int64_t)len;
}

static const struct kobject_ops file_ops = {
    .read_at = file_read_at,
    .write_at = file_write_at,
    .lseek = file_lseek,
    .close = NULL,
};

static const struct kobject_ops console_ops = {
    .read_at = NULL,
    .write_at = console_write_at,
    .lseek = NULL,
    .close = NULL,
};

int fd_install_stdio(struct task *t) {
    static bool console_ready = false;

    if (!console_ready) {
        kobject_init(&the_console.obj, KOBJ_CONSOLE, FD_RIGHT_READ | FD_RIGHT_WRITE, &console_ops);
        console_ready = true;
    }

    int in = fd_alloc(t, &the_console.obj, FD_RIGHT_READ, FD_FLAG_CLOEXEC);
    int out = fd_alloc(t, &the_console.obj, FD_RIGHT_WRITE, FD_FLAG_CLOEXEC);
    int err = fd_alloc(t, &the_console.obj, FD_RIGHT_WRITE, FD_FLAG_CLOEXEC);

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
    if (node.is_dir) return FD_EISDIR;

    uint32_t rights = 0;
    if (oflags & FD_OPEN_WRITE) rights |= FD_RIGHT_WRITE;
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