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
#define ABI_ESPIPE  (-29)
#define ABI_ETIMEDOUT (-110)
#define ABI_EMFILE  (-24)
#define ABI_ENOSPC  (-28)
#define ABI_ENOMEM  (-12)
#define ABI_ECHILD  (-10)
#define ABI_WNOHANG 1
#define ABI_ENOSYS  (-38)
#define ABI_EBADTYPE (-91)

#define ABI_O_ACCMODE   0x3
#define ABI_O_RDONLY    0x0
#define ABI_O_WRONLY    0x1
#define ABI_O_RDWR      0x2
#define ABI_O_CREAT     0x40
#define ABI_O_TRUNC     0x200
#define ABI_O_DIRECTORY 0x10000
#define ABI_O_NONBLOCK  0x800

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

struct sys_timespec {
    int64_t sec;
    int64_t nsec;
};

struct sys_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct sys_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ibaud;
    uint32_t c_obaud;
};

struct sys_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#define ABI_POLLIN   0x001
#define ABI_POLLOUT  0x004
#define ABI_POLLERR  0x008
#define ABI_POLLHUP  0x010
#define ABI_POLLNVAL 0x020

#define ABI_TCGETATTR 0
#define ABI_TCSETATTR 1
#define ABI_TCSANOW   0
#define ABI_TCSADRAIN 1
#define ABI_TCSaflUSH 2

#define ABI_CLOCK_REALTIME  0
#define ABI_CLOCK_MONOTONIC 1

#define ABI_MMAP_PROT_READ   1
#define ABI_MMAP_PROT_WRITE  2
#define ABI_MMAP_PROT_EXEC   4
#define ABI_MMAP_ANON       0x20

#define ABI_FUTEX_WAIT 0
#define ABI_FUTEX_WAKE 1

#define ABI_CLONE_VM        0x00000100ULL
#define ABI_CLONE_FS        0x00000200ULL
#define ABI_CLONE_FILES     0x00000400ULL
#define ABI_CLONE_SIGHAND   0x00000800ULL
#define ABI_CLONE_THREAD    0x00010000ULL
#define ABI_CLONE_PARENT_SETTID 0x00100000ULL
#define ABI_CLONE_CHILD_CLEARTID  0x00200000ULL
#define ABI_CLONE_SYSVSEM   0x00040000ULL
#define ABI_CLONE_SETTLS    0x00080000ULL

#define ABI_AT_NULL    0
#define ABI_AT_PHDR    3
#define ABI_AT_PHENT   4
#define ABI_AT_PHNUM   5
#define ABI_AT_PAGESZ  6
#define ABI_AT_ENTRY   9
#define ABI_AT_RANDOM  25

#include "syscalls.def"

#define X(name, num, ...) SYS_##name = num,
enum syscall_nr { SYSCALLS SYSCALL_NR_MAX = 256 };
#undef X

#define SYSCALL_NR_COUNT (SYSCALL_NR_MAX)

#endif
