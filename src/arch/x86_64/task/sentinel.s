.intel_syntax noprefix

.global sentinel_test
.type sentinel_test,@function
sentinel_test:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov rax, 0xA0A0A0A0A0A0A0A0
    mov rbx, 0x1111111111111111
    mov rcx, 0xC0C0C0C0C0C0C0C0
    mov rdx, 0xD0D0D0D0D0D0D0D0
    mov rsi, 0xE0E0E0E0E0E0E0E0
    mov rdi, 0xF0F0F0F0F0F0F0F0
    mov rbp, 0x0101010101010101
    mov r8,  0x0202020202020202
    mov r9,  0x0303030303030303
    mov r10, 0x0404040404040404
    mov r11, 0x0505050505050505
    mov r12, 0x0606060606060606
    mov r13, 0x0707070707070707
    mov r14, 0x0808080808080808
    mov r15, 0x0909090909090909

    mov qword ptr [rip + sentinel_counter], 80000000

.spin:
    dec qword ptr [rip + sentinel_counter]
    jnz .spin

    mov [rip + sentinel_regs + 0], rax
    mov [rip + sentinel_regs + 8], rbx
    mov [rip + sentinel_regs + 16], rcx
    mov [rip + sentinel_regs + 24], rdx
    mov [rip + sentinel_regs + 32], rsi
    mov [rip + sentinel_regs + 40], rdi
    mov [rip + sentinel_regs + 48], rbp
    mov [rip + sentinel_regs + 56], r8
    mov [rip + sentinel_regs + 64], r9
    mov [rip + sentinel_regs + 72], r10
    mov [rip + sentinel_regs + 80], r11
    mov [rip + sentinel_regs + 88], r12
    mov [rip + sentinel_regs + 96], r13
    mov [rip + sentinel_regs + 104], r14
    mov [rip + sentinel_regs + 112], r15

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
