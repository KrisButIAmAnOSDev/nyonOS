#ifndef FD_H
#define FD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/fs/fs.h"

#define FD_INLINE      16
#define FD_MAX_TOTAL   256
#define FD_GEN_SHIFT   24
#define FD_INDEX_MASK  ((1 << FD_GEN_SHIFT) - 1)
#define FD_GEN_MASK    0x7F

#define FD_RIGHT_READ     (1u << 0)
#define FD_RIGHT_WRITE    (1u << 1)
#define FD_RIGHT_DUP      (1u << 2)
#define FD_RIGHT_TRANSFER (1u << 3)
#define FD_RIGHTS_ALL     0x0F

#define FD_FLAG_CLOEXEC  (1u << 0)
#define FD_FLAG_NONBLOCK (1u << 1)

#define FD_OPEN_READ   0x1
#define FD_OPEN_WRITE  0x2
#define FD_OPEN_CREATE 0x4
#define FD_OPEN_TRUNC  0x8

#define FD_SEEK_SET 0
#define FD_SEEK_CUR 1
#define FD_SEEK_END 2

#define KOBJ_MAGIC       0x6B6F6201UL
#define KOBJ_MAGIC_FREE  0xDEADDEADUL
#define KOBJ_REF_SATURATE 0xFFFFFFFEUL

#define FD_EBADF    (-9)
#define FD_EBADTYPE (-91)
#define FD_EINVAL   (-22)
#define FD_EFAULT   (-14)
#define FD_ENOENT   (-2)
#define FD_EMFILE   (-24)
#define FD_EACCES   (-13)
#define FD_ENOSPC   (-28)
#define FD_ENOSYS   (-38)
#define FD_ENOTDIR  (-20)
#define FD_EISDIR   (-21)
#define FD_EIO      (-5)

#define KOBJ_TYPE_ANY 0
#define KOBJ_FILE    1
#define KOBJ_CONSOLE 2

struct kobject;

struct kobject_ops {
    int64_t (*read_at)(struct kobject *o, void *ubuf, uint32_t len, uint64_t off);
    int64_t (*write_at)(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off);
    int64_t (*lseek)(struct kobject *o, int64_t off, int whence);
    int (*close)(struct kobject *o);
};

struct kobject {
    uint32_t magic;
    uint32_t type;
    uint32_t refcount;
    uint32_t max_rights;
    const struct kobject_ops *ops;
};

struct file {
    struct kobject obj;
    fs_node_t node;
    uint64_t pos;
    bool writable;
};

struct console {
    struct kobject obj;
};

struct fd_entry {
    struct kobject *obj;
    uint16_t rights;
    uint16_t flags;
    uint8_t generation;
};

struct fd_table {
    struct fd_entry inl[FD_INLINE];
    struct fd_entry *heap;
    uint32_t capacity;
    uint64_t freebits[4];
    bool grown;
};

struct task;

void kobject_init(struct kobject *o, uint32_t type, uint32_t max_rights,
                  const struct kobject_ops *ops);
void kobject_get(struct kobject *o);
void kobject_put(struct kobject *o);
bool kobject_valid(const struct kobject *o);

void fd_table_init(struct task *t);
void fd_table_clear(struct task *t);

int fd_alloc(struct task *t, struct kobject *obj, uint32_t rights, uint32_t flags);
int fd_install_stdio(struct task *t);
int fd_close(struct task *t, int fd);
int fd_dup(struct task *t, int fd, uint32_t rights);

struct kobject *fd_get_checked(struct task *t, int fd, uint32_t type, uint32_t needed);
void fd_put(struct kobject *o);

int64_t fd_read(struct task *t, int fd, void *ubuf, uint32_t len);
int64_t fd_write(struct task *t, int fd, const void *ubuf, uint32_t len);
int64_t fd_lseek(struct task *t, int fd, int64_t off, int whence);
int fd_open(struct task *t, const char *path, uint32_t oflags);

#endif