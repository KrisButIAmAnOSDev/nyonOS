#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/x86_64/idt/idt.h"

#define SYS_MAX_IO 65536u

extern uint64_t syscall_kstack_top;

#define SYSCALL_VECTOR 0x80
#define SYSCALL_NR_MAX 9

#define SYS_WRITE    1
#define SYS_EXIT     2
#define SYS_OPEN     3
#define SYS_CLOSE    4
#define SYS_READ     5
#define SYS_FD_WRITE 6
#define SYS_LSEEK    7
#define SYS_DUP      8

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
#define FD_CHECKS    16

void syscall_init(void);
void syscall_dispatch(struct isr_frame *frame);

#endif
