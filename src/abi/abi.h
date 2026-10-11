#ifndef ABI_H
#define ABI_H

#include <stdint.h>
#include <stddef.h>

#define ABI_MAX_NAME 32

#define ABI_EPERM   (-1)
#define ABI_ENOENT  (-2)
#define ABI_ESRCH   (-3)
#define ABI_EIO     (-5)
#define ABI_EBADF   (-9)
#define ABI_EAGAIN  (-11)
#define ABI_EACCES  (-13)
#define ABI_EFAULT  (-14)
#define ABI_ENOTDIR (-20)
#define ABI_EISDIR  (-21)
#define ABI_EINVAL  (-22)
#define ABI_EMFILE  (-24)
#define ABI_ENOSPC  (-28)
#define ABI_ENOSYS  (-38)
#define ABI_EBADTYPE (-91)

#define ABI_O_RDONLY    0x1
#define ABI_O_WRONLY    0x2
#define ABI_O_RDWR      0x3
#define ABI_O_CREAT     0x4
#define ABI_O_TRUNC     0x8
#define ABI_O_DIRECTORY 0x10
#define ABI_O_NONBLOCK  0x800

#define ABI_PRINT_NONE 3

#define ABI_SEEK_SET 0
#define ABI_SEEK_CUR 1
#define ABI_SEEK_END 2

#define ABI_F_DUPFD 0
#define ABI_F_GETFD 1
#define ABI_F_SETFD 2
#define ABI_F_GETFL 3
#define ABI_F_SETFL 4

#define ABI_KTTY_GETDEST 0x5401
#define ABI_KTTY_SETDEST 0x5402
#define ABI_KTTY_GETFG   0x540F
#define ABI_KTTY_SETFG   0x5410

#define ABI_FD_RIGHT_READ 1

struct sys_stat {
    uint64_t size;
    uint64_t cluster;
    uint32_t attr;
    uint32_t is_dir;
    char name[ABI_MAX_NAME];
};

struct sys_dirent {
    uint64_t size;
    uint32_t cluster;
    uint32_t attr;
    uint32_t is_dir;
    char name[ABI_MAX_NAME];
};

#include "syscalls.def"

#define X(name, num, ...) SYS_##name = num,
enum syscall_nr { SYSCALLS SYSCALL_NR_MAX = 256 };
#undef X

#define SYSCALL_NR_COUNT (SYSCALL_NR_MAX)

#endif
