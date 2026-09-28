.intel_syntax noprefix

.global task_resume_asm
.type task_resume_asm,@function
task_resume_asm:
    mov r11, rdi

    mov r15, [r11+0]
    mov r14, [r11+8]
    mov r13, [r11+16]
    mov r12, [r11+24]
    mov r10, [r11+40]
    mov r9,  [r11+48]
    mov r8,  [r11+56]
    mov rbp, [r11+64]
    mov rdi, [r11+72]
    mov rsi, [r11+80]
    mov rdx, [r11+88]
    mov rcx, [r11+96]
    mov rbx, [r11+104]
    mov rax, [r11+112]

    lea rsp, [r11+136]
    mov r11, [rsp-104]
    iretq
