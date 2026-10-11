.intel_syntax noprefix

.extern syscall_dispatch
#include "percpu_off.h"

.set FRAME_RIP, 17*8

.global syscall_entry
.type syscall_entry,@function
syscall_entry:
    mov  r11, rsp
    mov  rsp, gs:PERCPU_SYSCALL_KSTACK_OFF

    push 0x1b
    push r11
    pushfq
    push 0x23
    push rcx
    push 0
    push 0x80
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov  rdi, rsp
    call syscall_dispatch

    mov  rax, [rsp + FRAME_RIP]
    shr  rax, 47
    jnz  .slow_return

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    add  rsp, 16
    mov  rcx, [rsp]
    mov  r11, [rsp + 16]
    mov  rsp, [rsp + 24]
    sysretq

.slow_return:
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    add  rsp, 16
    iretq
