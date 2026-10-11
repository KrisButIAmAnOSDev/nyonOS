#ifndef SYS_H
#define SYS_H

#include <stdint.h>
#include "abi.h"

typedef unsigned long ulong;
typedef long slong;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

#define O_RDONLY     ABI_O_RDONLY
#define O_WRONLY     ABI_O_WRONLY
#define O_RDWR       ABI_O_RDWR
#define O_CREAT      ABI_O_CREAT
#define O_TRUNC      ABI_O_TRUNC
#define O_DIRECTORY  ABI_O_DIRECTORY
#define O_NONBLOCK   ABI_O_NONBLOCK

#define PRINT_NONE ABI_PRINT_NONE

#define SEEK_SET ABI_SEEK_SET
#define SEEK_CUR ABI_SEEK_CUR
#define SEEK_END ABI_SEEK_END

#define F_DUPFD ABI_F_DUPFD
#define F_GETFD ABI_F_GETFD
#define F_SETFD ABI_F_SETFD
#define F_GETFL ABI_F_GETFL
#define F_SETFL ABI_F_SETFL

#define KTTY_GETDEST ABI_KTTY_GETDEST
#define KTTY_SETDEST ABI_KTTY_SETDEST
#define KTTY_GETFG   ABI_KTTY_GETFG
#define KTTY_SETFG   ABI_KTTY_SETFG

#define FD_RIGHT_READ ABI_FD_RIGHT_READ

#define EPERM   ABI_EPERM
#define ENOENT  ABI_ENOENT
#define ESRCH   ABI_ESRCH
#define EIO     ABI_EIO
#define EBADF   ABI_EBADF
#define EAGAIN  ABI_EAGAIN
#define EACCES  ABI_EACCES
#define EFAULT  ABI_EFAULT
#define ENOTDIR ABI_ENOTDIR
#define EISDIR  ABI_EISDIR
#define EINVAL  ABI_EINVAL
#define EMFILE  ABI_EMFILE
#define ENOSPC  ABI_ENOSPC
#define ENOSYS  ABI_ENOSYS

#define SYS_MAX_NAME ABI_MAX_NAME

static slong __sys0(ulong n, ulong unused) {
    (void)unused;
    slong r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static slong __sys1(ulong n, ulong a0) {
    slong r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a0) : "rcx", "r11", "memory");
    return r;
}

static slong __sys2(ulong n, ulong a0, ulong a1) {
    slong r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return r;
}

static slong __sys3(ulong n, ulong a0, ulong a1, ulong a2) {
    slong r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return r;
}

static slong __sys4(ulong n, ulong a0, ulong a1, ulong a2, ulong a3) {
    slong r;
    register ulong r10 __asm__("r10") = a3;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

#define N(...) __VA_ARGS__

#define X(name, num, ret, nargs, argnames, ...)                                        \
    static inline ret sys_##name(__VA_ARGS__) {                                         \
        return (ret)__sys##nargs(num, argnames);                                        \
    }

SYSCALLS
#undef X
#undef N

static inline int64_t sys_raw(unsigned long nr) { return (int64_t)__sys1(nr, 0); }


#endif
