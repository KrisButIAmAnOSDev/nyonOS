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

.macro WRITE_LEN dest, colour, begin, length
    mov rax, 1
    mov rdi, \dest
    mov rsi, \colour
    lea rdx, [rip + \begin]
    mov r10, \length
    int 0x80
.endm

.global user_test_entry
.type user_test_entry,@function
user_test_entry:

    WRITE 2, 0x00ffff, msg, msg_end

    mov rax, 2
    mov rdi, 0
    int 0x80

.global user_validate_entry
.type user_validate_entry,@function
user_validate_entry:

    mov rax, 1
    mov rdi, 0
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    mov r10, 4
    int 0x80

    mov qword ptr [0x60000000 + 0], rax

    mov rax, 1
    mov rdi, 9
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    mov r10, 4
    int 0x80

    mov qword ptr [0x60000000 + 8], rax

    mov rax, 1
    mov rdi, 0
    mov rsi, 0xffffff
    xor rdx, rdx
    mov r10, 4
    int 0x80

    mov qword ptr [0x60000000 + 16], rax

    mov rax, 1
    mov rdi, 0
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    xor r10d, r10d
    int 0x80

    mov qword ptr [0x60000000 + 24], rax

    mov rax, 1
    mov rdi, 0
    mov rsi, 0xffffff
    mov rdx, 0xffffffff80000000
    mov r10, 8
    int 0x80

    mov qword ptr [0x60000000 + 32], rax

    mov rax, 1
    mov rdi, 0
    mov rsi, 0xffffff
    mov rdx, 0x10
    mov r10, 8
    int 0x80

    mov qword ptr [0x60000000 + 40], rax

    mov rax, 1
    mov rdi, 0
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    mov r10, 0xffff
    int 0x80

    mov qword ptr [0x60000000 + 48], rax

    mov rax, 1
    mov rdi, 0
    mov rsi, 0xffffff
    mov rdx, 0x3fffffff
    mov r10, 2
    int 0x80

    mov qword ptr [0x60000000 + 56], rax

    mov rax, 77
    int 0x80

    mov qword ptr [0x60000000 + 64], rax

    mov qword ptr [0x60000000 + 72], 1

    WRITE 2, 0x00ffff, vdone, vdone_end

    mov rax, 2
    mov rdi, 0
    int 0x80

.spin2:
    jmp .spin2

msg:
    .ascii "nyawnyaw nyoe nyalsei nyoo\n"
msg_end:

vmsg:
    .ascii "nope"
vmsg_end:

vdone:
    .ascii "nyawnyaw nyoe nyalsei nyoo validation done\n"
vdone_end:

.global user_fault_entry
.type user_fault_entry,@function
user_fault_entry:

    WRITE 2, 0x00ff00, fault_msg, fault_msg_end

    mov rax, 0
    mov rbx, [rax]

    mov rax, 2
    mov rdi, 0
    int 0x80

.spin3:
    jmp .spin3

fault_msg:
    .ascii "nyawnyaw nyoe nyalsei nyoo fault\n"
fault_msg_end:

.global user_test_end
user_test_end:
