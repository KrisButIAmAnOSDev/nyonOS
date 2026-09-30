#ifndef SYSCALL_MSR_H
#define SYSCALL_MSR_H

#include <stdint.h>
#include "arch/x86_64/gdt/tss.h"

#define MSR_SYSENTER_CS       0x174
#define MSR_EFER              0xC0000080
#define MSR_STAR              0xC0000081
#define MSR_LSTAR             0xC0000082
#define MSR_SYSCALL_SFMASK    0xC0000084
#define MSR_SYSCALL_KERNEL_GS 0xC0000102

#define SYSCALL_SFMASK_VALUE  0x700

struct syscall_frame {
    uint64_t r15, r14, r13, r12, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t r11;
};

void syscall_msr_init(void);

extern uint64_t syscall_user_rsp;
extern struct tss *tss_rsp0_ptr;

#endif
