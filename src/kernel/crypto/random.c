#include <string.h>
#include "kernel/fd/fd.h"
#include "random.h"
#include "kernel/mm/heap/heap.h"
#include "kernel/sync/preempt.h"
#include "kernel/multitask/task.h"
#include "kernel/uaccess/uaccess.h"
#include "kernel/crypto/crypto.h"
#include "abi/abi.h"

#define RANDOM_MAX_READ 4096

static int64_t random_read_at(struct kobject *o, void *ubuf, uint32_t len, uint64_t off) {
    (void)o;
    (void)off;
    if (len == 0) return 0;
    if (len > RANDOM_MAX_READ) len = RANDOM_MAX_READ;

    uint8_t buf[RANDOM_MAX_READ];
    crypto_random_bytes(buf, len);

    if (!copyout(ubuf, buf, len)) return FD_EFAULT;
    return (int64_t)len;
}

static int64_t random_write_at(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off) {
    (void)o;
    (void)ubuf;
    (void)off;
    return (int64_t)len;
}

static bool random_poll_read(struct kobject *o, uint16_t events, uint16_t *revents) {
    (void)o;
    if (events & ABI_POLLIN) *revents |= ABI_POLLIN;
    if (events & ABI_POLLOUT) *revents |= ABI_POLLOUT;
    return *revents != 0;
}

static const struct kobject_ops random_ops = {
    .read_at = random_read_at,
    .write_at = random_write_at,
    .poll_ready = random_poll_read,
};

int random_create(uint16_t *out_flags) {
    struct task *t = task_current();
    if (!t) return FD_EINVAL;

    struct kobject *o = (struct kobject *)kmalloc(sizeof(struct kobject));
    if (!o) return FD_ENOMEM;
    memset(o, 0, sizeof(struct kobject));

    kobject_init(o, KOBJ_RANDOM,
                 FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, &random_ops);
    o->user_bufs = true;

    if (out_flags) *out_flags = 0;

    int fd = fd_alloc(t, o,
                      FD_RIGHT_READ | FD_RIGHT_WRITE | FD_RIGHT_DUP, 0);
    kobject_put(o);
    if (fd < 0) kobject_free(o);
    return fd;
}
