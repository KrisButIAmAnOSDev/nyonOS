#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/x86_64/idt/idt.h"

#define SYSCALL_VECTOR 0x80
#define SYSCALL_NR_MAX 3

#define SYS_WRITE 1
#define SYS_EXIT  2

#define SYS_EFAULT  (-14)
#define SYS_EINVAL  (-22)
#define SYS_ENOSYS  (-38)

void syscall_init(void);
void syscall_dispatch(struct isr_frame *frame);

#endif
