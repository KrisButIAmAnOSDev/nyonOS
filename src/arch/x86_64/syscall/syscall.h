#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/x86_64/idt/idt.h"

#define SYS_MAX_IO 65536u

extern uint64_t syscall_kstack_top;

#define SYSCALL_VECTOR 0x80
#define SYSCALL_NR_MAX 256

#define SYS_MAX_NAME 32


#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_PREAD       3
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_LSEEK       6
#define SYS_DUP         7
#define SYS_FSTAT       8
#define SYS_GETDENTS    9
#define SYS_ISATTY     10
#define SYS_STAT       11

#define SYS_EXIT       16
#define SYS_GETPID     17
#define SYS_SLEEP      18


#define SYS_CONSOLE_WRITE 240
#define SYS_DEBUG_PRINT   241

struct sys_stat {
    uint64_t size;
    uint64_t cluster;
    uint32_t attr;
    uint32_t is_dir;
    char name[SYS_MAX_NAME];
};

struct sys_dirent {
    uint64_t size;
    uint32_t cluster;
    uint32_t attr;
    uint32_t is_dir;
    char name[SYS_MAX_NAME];
};

#define SYS_EPERM   (-1)
#define SYS_EBADF   (-9)
#define SYS_EFAULT  (-14)
#define SYS_EINVAL  (-22)
#define SYS_ENOSYS  (-38)
#define SYS_ENOENT  (-2)
#define SYS_EMFILE  (-24)
#define SYS_EISDIR  (-21)

#define FD_OPEN_READ  0x1
#define FD_OPEN_WRITE 0x2

#define FD_SEEK_SET 0
#define FD_SEEK_CUR 1
#define FD_SEEK_END 2

#define FD_RIGHT_READ     (1u << 0)
#define FD_RIGHT_WRITE    (1u << 1)
#define FD_RIGHT_DUP      (1u << 2)
#define FD_RIGHT_TRANSFER (1u << 3)
#define FD_RIGHTS_ALL     0x0F

#define FD_RES_SLOTS 40
#define FD_RES_DONE  39
#define FD_CHECKS    25

void syscall_init(void);
void syscall_dispatch(struct isr_frame *frame);

#endif
