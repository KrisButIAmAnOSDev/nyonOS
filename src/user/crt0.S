.intel_syntax noprefix

.extern main
.extern __start_got
.extern __stop_got

.section .text.start, "ax"
.global _start
.type _start, @function
_start:
    xor rbp, rbp

    mov rbx, rsp

    lea r13, [rip]
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

    call main

    mov edi, eax
    mov rax, 16
    syscall

    mov rax, 231
    syscall

.hang:
    jmp .hang
