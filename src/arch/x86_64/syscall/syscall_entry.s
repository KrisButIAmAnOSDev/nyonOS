.intel_syntax noprefix

.global syscall_entry
.type syscall_entry,@function
syscall_entry:

    push rax
    mov  rax, rsp
    sub  rax, 8
    mov  [rip + syscall_user_rsp], rax
    pop  rax

    mov  rdx, [rip + tss_rsp0_ptr]
    mov  rsp, [rdx + 4]

    push 0x18
    push qword ptr [rip + syscall_user_rsp]
    push r11
    push 0x20
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
    sysretq
