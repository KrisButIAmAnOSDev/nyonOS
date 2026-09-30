.intel_syntax noprefix

.macro WRITE dest, colour, begin, end
    mov rax, 1
    mov rdi, \dest
    mov rsi, \colour
    lea rdx, [rip + \begin]
    lea r10, [rip + \end]
    sub r10, rdx
    int 0x80
.endm

.global user_test_entry
.type user_test_entry,@function
user_test_entry:

    WRITE 2, 0x00ffff, msg, msg_end

    mov rax, 2
    int 0x80

.spin:
    jmp .spin

msg:
    .ascii "nyawnyaw nyoe nyalsei nyoo\n"
msg_end:

.global user_test_end
user_test_end:
