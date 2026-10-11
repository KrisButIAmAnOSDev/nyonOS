#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/x86_64/idt/idt.h"

#define SYS_MAX_IO 65536u


#define SYSCALL_VECTOR 0x80

#include "abi/abi.h"

#define SYS_MAX_NAME ABI_MAX_NAME

#define SYS_EPERM    ABI_EPERM
#define SYS_ENOENT   ABI_ENOENT
#define SYS_ESRCH    ABI_ESRCH
#define SYS_EIO      ABI_EIO
#define SYS_EBADF    ABI_EBADF
#define SYS_EAGAIN   ABI_EAGAIN
#define SYS_EACCES   ABI_EACCES
#define SYS_EFAULT   ABI_EFAULT
#define SYS_ENOTDIR  ABI_ENOTDIR
#define SYS_EISDIR   ABI_EISDIR
#define SYS_EINVAL   ABI_EINVAL
#define SYS_EMFILE   ABI_EMFILE
#define SYS_ENOSPC   ABI_ENOSPC
#define SYS_ENOSYS   ABI_ENOSYS

#define FD_RES_SLOTS 144
#define FD_RES_DONE 64
#define FD_RES_FAIL 72
#define FD_CHECKS 56

#define C_RES_VIRT       0x60000000ULL
#define C_RES_DONE       0
#define C_RES_CHECK_BASE 1
#define C_RES_GOT_BASE   192
#define C_RES_FAIL_BASE  320
#define C_RES_SLOTS      512
#define C_MAX_CHECKS  (C_RES_GOT_BASE - C_RES_CHECK_BASE)
#define C_RES_STDIN_REQ 448
#define C_RES_STDIN_ACK 456
#define C_CHECKS       99

void syscall_init(void);
void syscall_dispatch(struct isr_frame *frame);

#endif
