#include "kernel/lib/string.h"
#include "kernel/fd/fd.h"
#include "kernel/fd/memfd.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"
#include "abi/abi.h"

struct memfd {
    struct kobject obj;
    uint8_t *data;
    size_t size;
    size_t cap;
    uint64_t off;
};

static int memfd_grow(struct memfd *mf, size_t need) {
    if (need <= mf->cap) return 0;
    size_t cap = mf->cap ? mf->cap : 64;
    while (cap < need) cap <<= 1;
    uint8_t *nd = (uint8_t *)krealloc(mf->data, cap);
    if (!nd) return FD_ENOMEM;
    memset(nd + mf->cap, 0, cap - mf->cap);
    mf->data = nd;
    mf->cap = cap;
    return 0;
}

static int64_t memfd_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    struct memfd *mf = (struct memfd *)o;
    if (mf->off >= mf->size) return 0;
    size_t n = len;
    if (n > mf->size - mf->off) n = mf->size - mf->off;
    if (!copyout(ubuf, mf->data + mf->off, n)) return FD_EFAULT;
    mf->off += n;
    return (int64_t)n;
}

static int64_t memfd_write_at(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off) {
    (void)off;
    struct memfd *mf = (struct memfd *)o;
    int r = memfd_grow(mf, mf->off + len);
    if (r) return r;
    if (!copyin(mf->data + mf->off, ubuf, len)) return FD_EFAULT;
    mf->off += len;
    if (mf->off > mf->size) mf->size = mf->off;
    poll_notify();
    return (int64_t)len;
}

static int64_t memfd_lseek(struct kobject *o, int64_t off, int whence) {
    struct memfd *mf = (struct memfd *)o;
    int64_t base = 0;
    if (whence == ABI_SEEK_CUR) base = (int64_t)mf->off;
    else if (whence == ABI_SEEK_END) base = (int64_t)mf->size;
    else if (whence != ABI_SEEK_SET) return FD_EINVAL;
    int64_t n = base + off;
    if (n < 0) return FD_EINVAL;
    mf->off = (uint64_t)n;
    return n;
}

static int memfd_close(struct kobject *o) {
    struct memfd *mf = (struct memfd *)o;
    if (mf->data) kfree(mf->data);
    return 0;
}

static bool memfd_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    struct memfd *mf = (struct memfd *)o;
    if ((events & ABI_POLLIN) && mf->off < mf->size) *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static const struct kobject_ops memfd_ops = {
    .read_at = memfd_read_at,
    .write_at = memfd_write_at,
    .lseek = memfd_lseek,
    .close = memfd_close,
    .poll_ready = memfd_poll_read,
};

int memfd_create(uint32_t flags, uint16_t *out_flags) {
    (void)flags;
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct memfd *mf = (struct memfd *)kmalloc(sizeof(struct memfd));
    if (!mf) return FD_ENOMEM;
    memset(mf, 0, sizeof(struct memfd));

    kobject_init(&mf->obj, KOBJ_MEMFD,
                 FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, &memfd_ops);
    mf->obj.user_bufs = true;

    uint16_t fflags = 0;
    if (flags & ABI_MFD_CLOEXEC) fflags |= FD_FLAG_CLOEXEC;
    if (out_flags) *out_flags = fflags;

    int fd = fd_alloc(t, &mf->obj,
                      FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, fflags);
    kobject_put(&mf->obj);
    return fd;
}

int memfd_truncate(void *obj, uint64_t len) {
    struct memfd *mf = (struct memfd *)obj;
    if (len > mf->cap) {
        int r = memfd_grow(mf, len);
        if (r) return r;
    }
    if (mf->size > len) mf->size = len;
    else memset(mf->data + mf->size, 0, len - mf->size);
    if (mf->off > mf->size) mf->off = mf->size;
    poll_notify();
    return 0;
}
