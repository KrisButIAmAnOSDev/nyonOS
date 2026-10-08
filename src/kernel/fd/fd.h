#ifndef FD_H
#define FD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel/fs/fs.h"
#include "abi/abi.h"

#define FD_INLINE      16
#define FD_MAX_TOTAL   256
#define FD_GEN_SHIFT   24
#define FD_INDEX_MASK  ((1 << FD_GEN_SHIFT) - 1)
#define FD_GEN_MASK    0x7F

#define FD_RIGHT_READ     (1u << 0)
#define FD_RIGHT_WRITE    (1u << 1)
#define FD_RIGHT_DUP      (1u << 2)
#define FD_RIGHT_TRANSFER (1u << 3)
#define FD_RIGHT_GETDENT  (1u << 4)
#define FD_RIGHTS_ALL     0x1F

#define FD_FLAG_CLOEXEC  (1u << 0)
#define FD_FLAG_NONBLOCK (1u << 1)

#define FD_OPEN_READ     ABI_O_RDONLY
#define FD_OPEN_WRITE    ABI_O_WRONLY
#define FD_OPEN_CREATE   ABI_O_CREAT
#define FD_OPEN_TRUNC    ABI_O_TRUNC
#define FD_OPEN_NONBLOCK ABI_O_NONBLOCK
#define FD_OPEN_DIR      ABI_O_DIRECTORY

#define FD_SEEK_SET ABI_SEEK_SET
#define FD_SEEK_CUR ABI_SEEK_CUR
#define FD_SEEK_END ABI_SEEK_END

#define KOBJ_MAGIC       0x6B6F6201UL
#define KOBJ_MAGIC_FREE  0xDEADDEADUL
#define KOBJ_REF_SATURATE 0xFFFFFFFEUL

#define FD_EBADF    ABI_EBADF
#define FD_EBADTYPE ABI_EBADTYPE
#define FD_EINVAL   ABI_EINVAL
#define FD_EFAULT   ABI_EFAULT
#define FD_ENOENT   ABI_ENOENT
#define FD_EMFILE   ABI_EMFILE
#define FD_EACCES   ABI_EACCES
#define FD_ENOSPC   ABI_ENOSPC
#define FD_ENOSYS   ABI_ENOSYS
#define FD_ENOTDIR  ABI_ENOTDIR
#define FD_EISDIR   ABI_EISDIR
#define FD_EIO      ABI_EIO
#define FD_EAGAIN   ABI_EAGAIN
#define FD_EPERM    ABI_EPERM
#define FD_ESRCH    ABI_ESRCH

#define KOBJ_TYPE_ANY 0
#define KOBJ_FILE    1
#define KOBJ_CONSOLE 2

struct kobject;

struct kobject_ops {
    int64_t (*read_at)(struct kobject *o, void *ubuf, uint32_t len, uint64_t off);
    int64_t (*write_at)(struct kobject *o, const void *ubuf, uint32_t len, uint64_t off);
    int64_t (*lseek)(struct kobject *o, int64_t off, int whence);
    int (*close)(struct kobject *o);
    int (*wait_readable)(struct kobject *o);
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
    uint32_t dest;
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
int fd_dup2(struct task *t, int oldfd, int newfd, uint32_t flags);
int fd_dup_min(struct task *t, int oldfd, uint32_t min);
int fd_slot_in_use(struct task *t, int idx);
int fd_get_rights(struct task *t, int fd, uint16_t *rights);
int fd_get_status(struct task *t, int fd, uint16_t *flags);
int fd_set_status(struct task *t, int fd, uint16_t flags);
int fd_set_cloexec(struct task *t, int fd, bool on);
int fd_get_cloexec(struct task *t, int fd, bool *on);
int fd_console_dest(int fd, struct task *t, uint32_t *dest);
int fd_console_set_dest(int fd, struct task *t, uint32_t dest);
int fd_tty_get_fg(int fd, struct task *t, uint32_t *pid);
int fd_tty_set_fg(int fd, struct task *t, uint32_t pid);
void fd_tty_task_gone(uint32_t pid);

struct kobject *fd_get_checked(struct task *t, int fd, uint32_t type, uint32_t needed);
void fd_put(struct kobject *o);

int64_t fd_read(struct task *t, int fd, void *ubuf, uint32_t len);
int64_t fd_write(struct task *t, int fd, const void *ubuf, uint32_t len);
int64_t fd_lseek(struct task *t, int fd, int64_t off, int whence);
int fd_open(struct task *t, const char *path, uint32_t oflags);

#endif