.intel_syntax noprefix

.extern main
.extern sys_exit
.extern __libc_argc
.extern __libc_argv
.extern __libc_envp
.extern __libc_auxv
.extern __start_got
.extern __stop_got
.extern __libc_self_bias

.section .text.start, "ax"
.global _start
.type _start, @function
_start:
    xor rbp, rbp

    mov rbx, rsp

    lea r12, [rip + __libc_self_bias]
    mov r13, [r12]

    test r13, r13
    jz .no_bias

    lea rdi, [rip + __start_got]
    lea rcx, [rip + __stop_got]
    sub rcx, rdi
    shr rcx, 3

.relocate:
    test rcx, rcx
    jz .no_bias
    mov rax, [rdi]
    add rax, r13
    mov [rdi], rax
    add rdi, 8
    dec rcx
    jmp .relocate

.no_bias:
    mov rsp, rbx
    xor rbp, rbp

    mov rcx, [rsp]
    mov [rip + __libc_argc], rcx
    lea rax, [rsp + 8]
    mov [rip + __libc_argv], rax
    mov qword ptr [rip + __libc_envp], 0
    mov qword ptr [rip + __libc_auxv], 0

    call main

    mov edi, eax
    call sys_exit

    mov rax, 231
    syscall

.hang:
    jmp .hang

.section .rodata
dmsg:
    .ascii "\nraw stack word: "
dend:
    .ascii "\n"

.section .data
.align 8
.global __libc_self_bias
__libc_self_bias:
    .quad 0

.section .bss
.align 8
.global __libc_argc
__libc_argc:
    .quad 0
.global __libc_argv
__libc_argv:
    .quad 0
.global __libc_envp
__libc_envp:
    .quad 0
.global __libc_auxv
__libc_auxv:
    .quad 0
.global __libc_stack_top
__libc_stack_top:
    .quad 0
