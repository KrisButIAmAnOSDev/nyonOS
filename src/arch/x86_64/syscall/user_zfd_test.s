.intel_syntax noprefix

.set SYS_WRITE,    1
.set SYS_EXIT,     2
.set SYS_OPEN,     3
.set SYS_CLOSE,    4
.set SYS_READ,     5
.set SYS_FD_WRITE, 6
.set SYS_LSEEK,    7
.set SYS_DUP,      8

.set O_RDONLY,  1
.set SEEK_SET,  0

.set EBADF,  -9
.set ENOENT, -2
.set EFAULT, -14
.set EISDIR, -21

.set RIGHT_READ, 1

.set RES, 0x60000000

.macro PASS n
    mov qword ptr [RES + \n*8], 1
.endm

.macro FAIL n
    mov qword ptr [RES + 40*8 + \n*8], 1
    jmp report
.endm

.global user_zfd_entry
.type user_zfd_entry,@function
user_zfd_entry:

    mov rax, SYS_OPEN
    lea rdi, [rip + path_test]
    mov rsi, O_RDONLY
    int 0x80
    mov r12, rax
    test rax, rax
    js .fail_open
    PASS 0

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 14
    int 0x80
    cmp rax, 14
    jne .fail_readlen
    PASS 1

    lea rsi, [rip + want]
    lea rdi, [rip + fdbuf]
    xor ecx, ecx
.cmp_loop:
    mov al, [rdi + rcx]
    cmp al, [rsi + rcx]
    jne .fail_content
    test al, al
    jz .cmp_done
    inc rcx
    jmp .cmp_loop
.cmp_done:
    PASS 2

    mov rax, SYS_LSEEK
    mov rdi, r12
    xor rsi, rsi
    mov rdx, SEEK_SET
    int 0x80
    test rax, rax
    jnz .fail_seek
    PASS 3

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    int 0x80
    cmp rax, 4
    jne .fail_reread
    PASS 4

    mov rax, SYS_LSEEK
    mov rdi, r12
    mov rsi, 999999
    mov rdx, SEEK_SET
    int 0x80
    cmp rax, 3000
    jne .fail_clampeof
    PASS 5

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    int 0x80
    test rax, rax
    jnz .fail_eofzero
    PASS 6

    mov rax, SYS_DUP
    mov rdi, r12
    xor rsi, rsi
    mov rdx, RIGHT_READ
    int 0x80
    mov r14, rax
    test rax, rax
    js .fail_dup
    PASS 7

    mov rax, SYS_FD_WRITE
    mov rdi, r14
    lea rsi, [rip + msg_x]
    mov rdx, 1
    int 0x80
    cmp rax, EBADF
    jne .fail_rights
    PASS 8

    mov rax, SYS_CLOSE
    mov rdi, r14
    int 0x80
    test rax, rax
    jnz .fail_close
    PASS 9

    mov rax, SYS_CLOSE
    mov rdi, r12
    int 0x80
    test rax, rax
    jnz .fail_close2

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    int 0x80
    cmp rax, EBADF
    jne .fail_stale
    PASS 10

    mov rax, SYS_CLOSE
    mov rdi, r12
    int 0x80
    cmp rax, EBADF
    jne .fail_dblclose
    PASS 11

    mov rax, SYS_OPEN
    lea rdi, [rip + path_missing]
    mov rsi, O_RDONLY
    int 0x80
    cmp rax, ENOENT
    jne .fail_enoent
    PASS 12

    mov rax, SYS_OPEN
    lea rdi, [rip + path_dir]
    mov rsi, O_RDONLY
    int 0x80
    cmp rax, EISDIR
    jne .fail_eisdir
    PASS 13

    mov rax, SYS_READ
    mov rdi, 1
    mov rsi, 0xdeadbeef000
    mov rdx, 8
    int 0x80
    cmp rax, EFAULT
    jne .fail_efault
    PASS 14

    mov rax, SYS_FD_WRITE
    mov rdi, 1
    lea rsi, [rip + msg_hello]
    mov rdx, 6
    int 0x80
    cmp rax, 6
    jne .fail_stdout
    PASS 15

    mov rax, SYS_WRITE
    mov rdi, 2
    mov rsi, 0x00ffff
    lea rdx, [rip + msg_done]
    lea r10, [rip + msg_done_end]
    sub r10, rdx
    int 0x80

    mov qword ptr [RES + 39*8], 1
    mov rax, SYS_EXIT
    xor rdi, rdi
    int 0x80

.fail_open:      FAIL 0
.fail_readlen:   FAIL 1
.fail_content:   FAIL 2
.fail_seek:      FAIL 3
.fail_reread:    FAIL 4
.fail_clampeof:  FAIL 5
.fail_eofzero:   FAIL 6
.fail_dup:       FAIL 7
.fail_rights:    FAIL 8
.fail_close:     FAIL 9
.fail_close2:    FAIL 10
.fail_stale:     FAIL 10
.fail_dblclose:  FAIL 11
.fail_enoent:    FAIL 12
.fail_eisdir:    FAIL 13
.fail_efault:    FAIL 14
.fail_stdout:    FAIL 15

report:
    mov rax, SYS_FD_WRITE
    mov rdi, 1
    lea rsi, [rip + msg_fail]
    mov rdx, 8
    int 0x80

    mov qword ptr [RES + 39*8], 2
    mov rax, SYS_EXIT
    mov rdi, 1
    int 0x80

    .align 16
fdbuf:
    .space 256

path_test:
    .asciz "/TEST.TXT"
path_missing:
    .asciz "/NOPE.TXT"
path_dir:
    .asciz "/SUB"
want:
    .asciz "that my jarona"
msg_x:
    .asciz "x"
msg_hello:
    .ascii "hello\n"
msg_done:
    .ascii "fd ring3 ok\n"
msg_done_end:
msg_fail:
    .ascii "fd FAIL\n"

.size user_zfd_entry, . - user_zfd_entry

.global user_zfd_end
.type user_zfd_end,@object
.align 8
user_zfd_end:
