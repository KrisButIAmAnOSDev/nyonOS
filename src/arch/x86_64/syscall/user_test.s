.intel_syntax noprefix

.macro WRITE dest, colour, begin, end
    mov rax, 240
    mov rdi, \dest
    mov rsi, \colour
    lea rdx, [rip + \begin]
    lea r10, [rip + \end]
    sub r10, rdx
    syscall
.endm

.macro WRITE_LEN dest, colour, begin, length
    mov rax, 240
    mov rdi, \dest
    mov rsi, \colour
    lea rdx, [rip + \begin]
    mov r10, \length
    syscall
.endm



.global user_test_entry
.type user_test_entry,@function
user_test_entry:

    WRITE 2, 0x00ffff, msg, msg_end

    mov rax, 16
    mov rdi, 0
    syscall

.global user_validate_entry
.type user_validate_entry,@function
user_validate_entry:

    mov rax, 240
    mov rdi, 0
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    mov r10, 4
    syscall

    mov qword ptr [0x60000000 + 0], rax

    mov rax, 240
    mov rdi, 9
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    mov r10, 4
    syscall

    mov qword ptr [0x60000000 + 8], rax

    mov rax, 240
    mov rdi, 0
    mov rsi, 0xffffff
    xor rdx, rdx
    mov r10, 4
    syscall

    mov qword ptr [0x60000000 + 16], rax

    mov rax, 240
    mov rdi, 0
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    xor r10d, r10d
    syscall

    mov qword ptr [0x60000000 + 24], rax

    mov rax, 240
    mov rdi, 0
    mov rsi, 0xffffff
    mov rdx, 0xffffffff80000000
    mov r10, 8
    syscall

    mov qword ptr [0x60000000 + 32], rax

    mov rax, 240
    mov rdi, 0
    mov rsi, 0xffffff
    mov rdx, 0x10
    mov r10, 8
    syscall

    mov qword ptr [0x60000000 + 40], rax

    mov rax, 240
    mov rdi, 0
    mov rsi, 0xffffff
    lea rdx, [rip + vmsg]
    mov r10, 0xffff
    syscall

    mov qword ptr [0x60000000 + 48], rax

    mov rax, 240
    mov rdi, 0
    mov rsi, 0xffffff
    mov rdx, 0x3fffffff
    mov r10, 2
    syscall

    mov qword ptr [0x60000000 + 56], rax

    mov rax, 77
    syscall

    mov qword ptr [0x60000000 + 64], rax

    mov qword ptr [0x60000000 + 72], 1

    WRITE 2, 0x00ffff, vdone, vdone_end

    mov rax, 16
    mov rdi, 0
    syscall

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

    mov rax, 16
    mov rdi, 0
    syscall

.spin3:
    jmp .spin3

fault_msg:
    .ascii "nyawnyaw nyoe nyalsei nyoo fault\n"
fault_msg_end:

.set SYS_READ,          1
.set SYS_WRITE,         2
.set SYS_PREAD,         3
.set SYS_OPEN,          4
.set SYS_CLOSE,         5
.set SYS_LSEEK,         6
.set SYS_DUP,           7
.set SYS_FSTAT,         8
.set SYS_GETDENTS,     9
.set SYS_ISATTY,       10
.set SYS_STAT,         11
.set SYS_EXIT,         16
.set SYS_GETPID,       17
.set SYS_SLEEP,        18
.set SYS_CONSOLE_WRITE, 240
.set SYS_DEBUG_PRINT,  241

.set O_RDONLY,  1
.set O_DIRECTORY, 0x10
.set SEEK_SET,  0

.set EBADF,    -9
.set EPERM,    -1
.set ENOENT,   -2
.set EFAULT,  -14
.set EISDIR,  -21

.set RIGHT_READ, 1

.set RES, 0x60000000
.set WANT_SIZE, 3000
.set NCHECKS, 56

.macro PASS n
    mov qword ptr [RES + \n*8], 1
.endm

.macro FAIL n
    mov qword ptr [RES + (NCHECKS+16)*8 + \n*8], 1
    jmp report
.endm

.set SYS_DUP2,         12
.set SYS_FCNTL,        13
.set SYS_IOCTL,        14
.set F_DUPFD,   0
.set F_GETFD,    1
.set F_SETFD,    2
.set F_GETFL,    3
.set F_SETFL,    4
.set KTTY_GETDEST, 0x5401
.set KTTY_SETDEST, 0x5402

.macro ABI_PROBE fast, cfail, afail
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rip + abi_rsp], rsp
    mov rbx, 0x1111111111111111
    mov rbp, 0x2222222222222222
    mov r12, 0x3333333333333333
    mov r13, 0x4444444444444444
    mov r14, 0x5555555555555555
    mov r15, 0x6666666666666666
    mov rsi, 0x7777777777777777
    mov rdx, 0x8888888888888888
    mov r8,  0x9999999999999999
    mov r9,  0xaaaaaaaaaaaaaaaa
    mov r10, 0xbbbbbbbbbbbbbbbb
    mov rdi, 5
    mov rax, SYS_SLEEP
.if \fast
    syscall
.else
    int 0x80
.endif
    test rax, rax
    jnz \cfail
    cmp rsp, [rip + abi_rsp]
    jne \cfail
    mov rax, 0x1111111111111111
    cmp rbx, rax
    jne \cfail
    mov rax, 0x2222222222222222
    cmp rbp, rax
    jne \cfail
    mov rax, 0x3333333333333333
    cmp r12, rax
    jne \cfail
    mov rax, 0x4444444444444444
    cmp r13, rax
    jne \cfail
    mov rax, 0x5555555555555555
    cmp r14, rax
    jne \cfail
    mov rax, 0x6666666666666666
    cmp r15, rax
    jne \cfail
    cmp rdi, 5
    jne \afail
    mov rax, 0x7777777777777777
    cmp rsi, rax
    jne \afail
    mov rax, 0x8888888888888888
    cmp rdx, rax
    jne \afail
    mov rax, 0x9999999999999999
    cmp r8, rax
    jne \afail
    mov rax, 0xaaaaaaaaaaaaaaaa
    cmp r9, rax
    jne \afail
    mov rax, 0xbbbbbbbbbbbbbbbb
    cmp r10, rax
    jne \afail
    lea rdi, [rip + path_test]
    lea rsi, [rip + stbuf]
    mov rax, SYS_STAT
.if \fast
    syscall
.else
    int 0x80
.endif
    test rax, rax
    jnz \afail
    lea rax, [rip + path_test]
    cmp rdi, rax
    jne \afail
    lea rax, [rip + stbuf]
    cmp rsi, rax
    jne \afail
    mov rax, 0x8888888888888888
    cmp rdx, rax
    jne \afail
    mov rax, 0x9999999999999999
    cmp r8, rax
    jne \afail
    mov rax, 0xaaaaaaaaaaaaaaaa
    cmp r9, rax
    jne \afail
    mov rax, 0xbbbbbbbbbbbbbbbb
    cmp r10, rax
    jne \afail
    mov rax, 0x1111111111111111
    cmp rbx, rax
    jne \cfail
    mov rax, 0x6666666666666666
    cmp r15, rax
    jne \cfail
    cmp rsp, [rip + abi_rsp]
    jne \cfail
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
.endm

.global user_zfd_entry
.type user_zfd_entry,@function
user_zfd_entry:

    mov rax, SYS_OPEN
    lea rdi, [rip + path_test]
    mov rsi, O_RDONLY
    syscall
    mov r12, rax
    test rax, rax
    js .fail_open
    PASS 0

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 14
    syscall
    cmp rax, 14
    jne .fail_readlen
    PASS 1

    lea rsi, [rip + want]
    lea rdi, [rip + fdbuf]
    xor ecx, ecx
.cmp_read:
    mov al, [rdi + rcx]
    cmp al, [rsi + rcx]
    jne .fail_content
    test al, al
    jz .cmp_read_done
    inc rcx
    jmp .cmp_read
.cmp_read_done:
    PASS 2

    mov rax, SYS_PREAD
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 14
    mov r10, 0
    syscall
    cmp rax, 14
    jne .fail_preadlen
    PASS 16

    lea rsi, [rip + want]
    lea rdi, [rip + fdbuf]
    xor ecx, ecx
.cmp_pread:
    mov al, [rdi + rcx]
    cmp al, [rsi + rcx]
    jne .fail_preadcontent
    test al, al
    jz .cmp_pread_done
    inc ecx
    jmp .cmp_pread
.cmp_pread_done:
    PASS 17

    mov rax, SYS_FSTAT
    mov rdi, r12
    lea rsi, [rip + stbuf]
    syscall
    test rax, rax
    jnz .fail_fstat
    mov rax, [rip + stbuf]
    cmp rax, WANT_SIZE
    jne .fail_fstatsize
    PASS 18

    mov rax, SYS_STAT
    lea rdi, [rip + path_test]
    lea rsi, [rip + stbuf]
    syscall
    test rax, rax
    jnz .fail_stat
    mov rax, [rip + stbuf]
    cmp rax, WANT_SIZE
    jne .fail_statsize
    PASS 19

    mov rax, SYS_ISATTY
    mov rdi, r12
    syscall
    cmp rax, 0
    jne .fail_isattyfile
    PASS 20

    mov rax, SYS_ISATTY
    mov rdi, 1
    syscall
    cmp rax, 1
    jne .fail_isattytty
    PASS 21

    mov rax, SYS_OPEN
    lea rdi, [rip + path_dir]
    mov rsi, O_RDONLY | O_DIRECTORY
    syscall
    mov r15, rax
    test rax, rax
    js .fail_opendir
    PASS 25

    mov rax, SYS_READ
    mov rdi, r15
    lea rsi, [rip + fdbuf]
    mov rdx, 8
    syscall
    test rax, rax
    jz .fail_dirread

    mov rax, SYS_GETDENTS
    mov rdi, r15
    xor rsi, rsi
    lea rdx, [rip + dent]
    syscall
    cmp rax, 1
    jne .fail_getdents
    PASS 26

    mov rax, [rip + dent]
    cmp rax, 14
    jne .fail_getdentsize
    PASS 27

    mov rax, SYS_GETDENTS
    mov rdi, r15
    mov rsi, 99
    lea rdx, [rip + dent]
    syscall
    cmp rax, 0
    jne .fail_getdentsend
    PASS 28

    mov rax, SYS_CLOSE
    mov rdi, r15
    syscall

    mov rax, SYS_GETPID
    syscall
    test rax, rax
    jz .fail_getpid
    PASS 22

    mov rax, SYS_SLEEP
    mov rdi, 1
    syscall
    test rax, rax
    jnz .fail_sleep
    PASS 23

    mov rax, SYS_DEBUG_PRINT
    lea rdi, [rip + msg_x]
    mov rsi, 1
    syscall
    cmp rax, EPERM
    jne .fail_dbgprint
    PASS 24

    mov rax, SYS_DUP2
    mov rdi, 1
    mov rsi, 5
    xor rdx, rdx
    syscall
    cmp rax, 5
    jne .fail_dup2
    PASS 29

    mov rax, SYS_WRITE
    mov rdi, 5
    lea rsi, [rip + msg_ok]
    mov rdx, 2
    syscall
    cmp rax, 2
    jne .fail_dup2write
    PASS 30

    mov rax, SYS_ISATTY
    mov rdi, 5
    syscall
    cmp rax, 1
    jne .fail_dup2tty
    PASS 31

    mov rax, SYS_DUP2
    mov rdi, 99
    mov rsi, 6
    xor rdx, rdx
    syscall
    cmp rax, EBADF
    jne .fail_dup2bad
    PASS 32

    mov rax, SYS_DUP2
    mov rdi, 1
    mov rsi, 1
    xor rdx, rdx
    syscall
    cmp rax, 1
    jne .fail_dup2self
    PASS 33

    mov rax, SYS_FCNTL
    mov rdi, 5
    mov rsi, 1
    xor rdx, rdx
    syscall
    cmp rax, 1
    jne .fail_getfd
    PASS 34

    mov rax, SYS_FCNTL
    mov rdi, 5
    mov rsi, 2
    xor rdx, rdx
    syscall
    test rax, rax
    jnz .fail_setfd

    mov rax, SYS_FCNTL
    mov rdi, 5
    mov rsi, 1
    xor rdx, rdx
    syscall
    test rax, rax
    jnz .fail_setfd
    PASS 35

    mov rax, SYS_IOCTL
    mov rdi, 5
    mov rsi, 0x5401
    xor rdx, rdx
    syscall
    cmp rax, 2
    jne .fail_ioctl
    PASS 36

    mov rax, SYS_IOCTL
    mov rdi, 5
    mov rsi, 0x5402
    xor edx, edx
    syscall
    test rax, rax
    jnz .fail_ioctl

    mov rax, SYS_IOCTL
    mov rdi, 5
    mov rsi, 0x5401
    xor rdx, rdx
    syscall
    cmp rax, 0
    jne .fail_ioctl
    PASS 37

    mov rax, SYS_IOCTL
    mov rdi, 5
    mov rsi, 0x5402
    mov rdx, 2
    syscall
    test rax, rax
    jnz .fail_ioctl

    mov rax, SYS_IOCTL
    mov rdi, 5
    mov rsi, 0x5401
    xor rdx, rdx
    syscall
    cmp rax, 2
    jne .fail_ioctl
    PASS 38

    mov rax, SYS_READ
    mov rdi, 1
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    syscall
    cmp rax, EBADF
    jne .fail_readstdout
    PASS 39

    mov rax, SYS_FCNTL
    mov rdi, 1
    mov rsi, 3
    xor rdx, rdx
    syscall
    test rax, rax
    jz .fail_getfl
    PASS 40

    mov rax, SYS_FCNTL
    mov rdi, 1
    mov rsi, 0
    mov rdx, 20
    syscall
    cmp rax, 20
    jl .fail_dupfd
    mov r13, rax
    PASS 41

    mov rax, SYS_CLOSE
    mov rdi, r13
    syscall
    test rax, rax
    jnz .fail_dupfdclose

    mov rax, SYS_IOCTL
    mov rdi, 99
    mov rsi, 0x5401
    xor rdx, rdx
    syscall
    cmp rax, EBADF
    jne .fail_ioctlbad
    PASS 42

    mov rax, SYS_READ
    mov rdi, 2
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    syscall
    cmp rax, EBADF
    jne .fail_readstderr
    PASS 43

    mov rax, SYS_WRITE
    mov rdi, 0
    lea rsi, [rip + msg_x]
    mov rdx, 1
    syscall
    cmp rax, EBADF
    jne .fail_writestdin
    PASS 44

    mov rax, SYS_ISATTY
    mov rdi, 2
    syscall
    cmp rax, 1
    jne .fail_isattystderr
    PASS 45

    mov rax, SYS_DUP2
    mov rdi, 1
    mov rsi, 40
    xor rdx, rdx
    syscall
    cmp rax, 40
    jne .fail_dup2high
    mov r14, rax
    PASS 46

    mov rax, SYS_WRITE
    mov rdi, r14
    lea rsi, [rip + msg_x]
    mov rdx, 1
    syscall
    cmp rax, 1
    jne .fail_dup2highwrite
    PASS 47

    mov rax, SYS_CLOSE
    mov rdi, r14
    syscall
    test rax, rax
    jnz .fail_dup2highclose
    PASS 48

    mov rax, SYS_GETPID
    syscall
    push rax
    mov rax, SYS_GETPID
    int 0x80
    pop rdx
    cmp rax, rdx
    jne .fail_abi_pid
    test rax, rax
    jz .fail_abi_pid
    PASS 49

    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rip + msg_x]
    mov rdx, 1
    int 0x80
    cmp rax, 1
    jne .fail_abi_int80write
    PASS 50

    ABI_PROBE 1, .fail_abi_sc_callee, .fail_abi_sc_args
    PASS 51
    PASS 53

    ABI_PROBE 0, .fail_abi_int80, .fail_abi_int80
    PASS 52

    mov rax, SYS_GETPID
    syscall
.abi_after:
    lea rdx, [rip + .abi_after]
    cmp rcx, rdx
    jne .fail_abi_rcx
    bt r11, 9
    jnc .fail_abi_rcx
    PASS 54

    mov rax, 200
    syscall
    cmp rax, -38
    jne .fail_abi_enosys
    PASS 55

    mov rax, SYS_LSEEK
    mov rdi, r12
    xor rsi, rsi
    mov rdx, SEEK_SET
    syscall
    test rax, rax
    jnz .fail_seek
    PASS 3

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    syscall
    cmp rax, 4
    jne .fail_reread
    PASS 4

    mov rax, SYS_LSEEK
    mov rdi, r12
    mov rsi, 999999
    mov rdx, SEEK_SET
    syscall
    cmp rax, WANT_SIZE
    jne .fail_clampeof
    PASS 5

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    syscall
    test rax, rax
    jnz .fail_eofzero
    PASS 6

    mov rax, SYS_DUP
    mov rdi, r12
    xor rsi, rsi
    mov rdx, RIGHT_READ
    syscall
    mov r14, rax
    test rax, rax
    js .fail_dup
    PASS 7

    mov rax, SYS_WRITE
    mov rdi, r14
    lea rsi, [rip + msg_x]
    mov rdx, 1
    syscall
    cmp rax, EBADF
    jne .fail_rights
    PASS 8

    mov rax, SYS_CLOSE
    mov rdi, r14
    syscall
    test rax, rax
    jnz .fail_close
    PASS 9

    mov rax, SYS_CLOSE
    mov rdi, r12
    syscall
    test rax, rax
    jnz .fail_close2

    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rip + fdbuf]
    mov rdx, 4
    syscall
    cmp rax, EBADF
    jne .fail_stale
    PASS 10

    mov rax, SYS_CLOSE
    mov rdi, r12
    syscall
    cmp rax, EBADF
    jne .fail_dblclose
    PASS 11

    mov rax, SYS_OPEN
    lea rdi, [rip + path_missing]
    mov rsi, O_RDONLY
    syscall
    cmp rax, ENOENT
    jne .fail_enoent
    PASS 12

    mov rax, SYS_OPEN
    lea rdi, [rip + path_dir]
    mov rsi, O_RDONLY
    syscall
    cmp rax, EISDIR
    jne .fail_eisdir
    PASS 13

    mov rax, SYS_READ
    mov rdi, 1
    mov rsi, 0xdeadbeef000
    mov rdx, 8
    syscall
    cmp rax, EFAULT
    jne .fail_efault
    PASS 14

    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rip + msg_hello]
    mov rdx, 6
    syscall
    cmp rax, 6
    jne .fail_stdout
    PASS 15

    mov rax, SYS_CONSOLE_WRITE
    mov rdi, 2
    mov rsi, 0x00ffff
    lea rdx, [rip + msg_done]
    lea r10, [rip + msg_done_end]
    sub r10, rdx
    syscall

    mov qword ptr [RES + (NCHECKS+8)*8], 1

    mov rax, SYS_CONSOLE_WRITE
    mov rdi, 2
    mov rsi, 0x00ffff
    lea rdx, [rip + msg_prompt]
    lea r10, [rip + msg_prompt_end]
    sub r10, rdx
    syscall

    mov r13d, 0
.stdin_loop:
    mov rax, SYS_READ
    mov rdi, 0
    lea rsi, [rip + fdbuf]
    mov rdx, 32
    syscall
    test rax, rax
    jle .stdin_wait

    mov r14, rax

    mov rax, SYS_CONSOLE_WRITE
    mov rdi, 2
    mov rsi, 0x00ffaa
    lea rdx, [rip + msg_stdin_got]
    lea r10, [rip + msg_stdin_got_end]
    sub r10, rdx
    syscall

    mov rax, SYS_CONSOLE_WRITE
    mov rdi, 2
    mov rsi, 0xffffff
    lea rdx, [rip + fdbuf]
    mov r10, r14
    syscall

    mov rax, SYS_CONSOLE_WRITE
    mov rdi, 2
    mov rsi, 0x00ffaa
    lea rdx, [rip + msg_eol]
    lea r10, [rip + msg_eol_end]
    sub r10, rdx
    syscall

    mov rax, SYS_CONSOLE_WRITE
    mov rdi, 2
    mov rsi, 0x00ffaa
    lea rdx, [rip + msg_stdin_ok]
    lea r10, [rip + msg_stdin_ok_end]
    sub r10, rdx
    syscall

    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

.stdin_wait:
    mov rax, SYS_SLEEP
    mov rdi, 30
    syscall
    jmp .stdin_loop

.stdin_exit:
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

.fail_open:         FAIL 0
.fail_readlen:      FAIL 1
.fail_content:      FAIL 2
.fail_seek:         FAIL 3
.fail_reread:       FAIL 4
.fail_clampeof:     FAIL 5
.fail_eofzero:      FAIL 6
.fail_dup:          FAIL 7
.fail_rights:       FAIL 8
.fail_close:        FAIL 9
.fail_close2:       FAIL 10
.fail_stale:        FAIL 10
.fail_dblclose:     FAIL 11
.fail_enoent:       FAIL 12
.fail_eisdir:       FAIL 13
.fail_efault:       FAIL 14
.fail_stdout:       FAIL 15
.fail_preadlen:     FAIL 16
.fail_preadcontent: FAIL 17
.fail_fstat:        FAIL 18
.fail_fstatsize:    FAIL 18
.fail_stat:         FAIL 19
.fail_statsize:     FAIL 19
.fail_isattyfile:   FAIL 20
.fail_isattytty:    FAIL 21
.fail_getpid:       FAIL 22
.fail_sleep:        FAIL 23
.fail_dbgprint:     FAIL 24
.fail_dup2:         FAIL 29
.fail_dup2write:    FAIL 30
.fail_dup2tty:      FAIL 31
.fail_dup2bad:      FAIL 32
.fail_dup2self:     FAIL 33
.fail_getfd:        FAIL 34
.fail_setfd:        FAIL 35
.fail_ioctl:        FAIL 36
.fail_readstdout:   FAIL 39
.fail_getfl:        FAIL 40
.fail_dupfd:        FAIL 41
.fail_dupfdclose:   FAIL 41
.fail_ioctlbad:     FAIL 42
.fail_readstderr:   FAIL 43
.fail_writestdin:   FAIL 44
.fail_isattystderr: FAIL 45
.fail_dup2high:      FAIL 46
.fail_dup2highwrite: FAIL 47
.fail_dup2highclose: FAIL 48
.fail_abi_pid:       FAIL 49
.fail_abi_int80write: FAIL 50
.fail_abi_sc_callee: FAIL 51
.fail_abi_int80:     FAIL 52
.fail_abi_sc_args:   FAIL 53
.fail_abi_rcx:       FAIL 54
.fail_abi_enosys:    FAIL 55
.fail_opendir:      FAIL 25
.fail_getdents:     FAIL 26
.fail_getdentsize:  FAIL 27
.fail_getdentsend:  FAIL 28
.fail_dirread:      FAIL 26

report:
    mov rax, SYS_CONSOLE_WRITE
    mov rdi, 2
    mov rsi, 0x00ffff
    lea rdx, [rip + msg_fail]
    lea r10, [rip + msg_fail_end]
    sub r10, rdx
    syscall

    mov qword ptr [RES + (NCHECKS+8)*8], 2
    mov rax, SYS_EXIT
    mov rdi, 1
    syscall

    .align 16
fdbuf:
    .space 256
stbuf:
    .space 64
    .align 8
dent:
    .space 56
abi_rsp:
    .quad 0

path_test:
    .asciz "/TEST.TXT"
path_missing:
    .asciz "/NOPE.TXT"
path_dir:
    .asciz "/SUB"
want:
    .asciz "that my jarona"
msg_ok:
    .asciz "ok"
msg_x:
    .asciz "x"
msg_hello:
    .ascii "hello\n"
msg_done:
    .ascii "fd ring3 ok\n"
msg_done_end:
msg_fail:
    .ascii "fd FAIL\n"
msg_fail_end:
msg_prompt:
    .ascii "\n=== testing stdin: type something and press enter ===\n"
msg_prompt_end:


msg_stdin_ok:
    .ascii "=== stdin works ===\n"
msg_stdin_ok_end:
msg_stdin_got:
    .ascii "\nyou typed: "
msg_stdin_got_end:
msg_eol:
    .ascii "\n"
msg_eol_end:

.size user_zfd_entry, . - user_zfd_entry

.set EIO,     -5
.set ESRCH,   -3
.set EAGAIN, -11
.set KTTY_GETFG, 0x540F
.set KTTY_SETFG, 0x5410
.set TTY_DONE,  RES + 16*8
.set TTY_READY, RES + 17*8
.set TTY_GO,    RES + 18*8
.set TTY_ARG,   RES + 19*8
.set TTY_GOT,   RES + 32*8

.macro TEXPECT n
    je .Ltok\@
    mov qword ptr [TTY_GOT + \n*8], rax
    jmp tty_report
.Ltok\@:
    mov qword ptr [RES + \n*8], 1
.endm

.macro TGETFG fd
    mov rax, SYS_IOCTL
    mov rdi, \fd
    mov rsi, KTTY_GETFG
    xor rdx, rdx
    syscall
.endm

.macro TSETFG pid
    mov rax, SYS_IOCTL
    xor rdi, rdi
    mov rsi, KTTY_SETFG
    mov rdx, \pid
    syscall
.endm

.set O_NONBLOCK, 0x800

.macro TNONBLOCK on
    mov rax, SYS_FCNTL
    xor rdi, rdi
    mov rsi, F_SETFL
    mov rdx, \on
    syscall
.endm

.macro TREAD0
    mov rax, SYS_READ
    xor rdi, rdi
    lea rsi, [rip + ttybuf]
    mov rdx, 8
    syscall
.endm

.global user_tty_a_entry
.type user_tty_a_entry,@function
user_tty_a_entry:
    mov rax, SYS_GETPID
    syscall
    mov r12, rax
    TNONBLOCK O_NONBLOCK

    TREAD0
    cmp rax, EAGAIN
    TEXPECT 0

    TGETFG 0
    cmp rax, r12
    TEXPECT 1

    mov qword ptr [TTY_READY], r12

    mov r13, 400
.tty_a_wait:
    mov rax, [TTY_GO]
    cmp rax, 1
    je .tty_a_go
    mov rax, SYS_SLEEP
    mov rdi, 5
    syscall
    dec r13
    jnz .tty_a_wait
    mov rax, -1
    cmp rax, 0
    TEXPECT 2

.tty_a_go:
    TSETFG 0
    cmp rax, 0
    TEXPECT 2

    TGETFG 0
    cmp rax, 0
    TEXPECT 3

    TSETFG r12
    cmp rax, 0
    TEXPECT 4

    TSETFG 7777
    cmp rax, ESRCH
    TEXPECT 5

    TGETFG 1
    cmp rax, r12
    TEXPECT 6

    mov qword ptr [TTY_DONE], 1
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

.global user_tty_b_entry
.type user_tty_b_entry,@function
user_tty_b_entry:
    mov rax, SYS_GETPID
    syscall
    mov r12, rax
    mov r13, [TTY_ARG]

    TREAD0
    cmp rax, EIO
    TEXPECT 0

    TGETFG 0
    cmp rax, r13
    TEXPECT 1

    TSETFG r12
    cmp rax, EPERM
    TEXPECT 2

    TSETFG 0
    cmp rax, EPERM
    TEXPECT 3

    TGETFG 0
    cmp rax, r13
    TEXPECT 4

    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rip + msg_x]
    mov rdx, 1
    syscall
    cmp rax, 1
    TEXPECT 5

    TREAD0
    cmp rax, EIO
    TEXPECT 6

    mov qword ptr [TTY_DONE], 1
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

.global user_tty_c_entry
.type user_tty_c_entry,@function
user_tty_c_entry:
    mov rax, SYS_GETPID
    syscall
    mov r12, rax
    mov r13, [TTY_ARG]
    TNONBLOCK O_NONBLOCK

    TGETFG 0
    cmp rax, 0
    TEXPECT 0

    TREAD0
    cmp rax, EAGAIN
    TEXPECT 1

    TGETFG 0
    cmp rax, r12
    TEXPECT 2

    TSETFG r13
    cmp rax, ESRCH
    TEXPECT 3

    TGETFG 0
    cmp rax, r12
    TEXPECT 4

    mov qword ptr [TTY_DONE], 1
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

.set TTY_READY2, RES + 20*8
.set TTY_READY3, RES + 21*8
.set TTY_READY4, RES + 22*8
.set TTY_GO2,    RES + 23*8

.macro TREADN n
    mov rax, SYS_READ
    xor rdi, rdi
    lea rsi, [rip + ttybuf]
    mov rdx, \n
    syscall
.endm

.global user_wait_entry
.type user_wait_entry,@function
user_wait_entry:
    mov rax, SYS_FCNTL
    xor rdi, rdi
    mov rsi, F_GETFL
    xor rdx, rdx
    syscall
    test rax, O_NONBLOCK
    setz al
    movzx rax, al
    cmp rax, 1
    TEXPECT 0

    mov rax, SYS_FCNTL
    xor rdi, rdi
    mov rsi, F_SETFD
    xor rdx, rdx
    syscall
    TNONBLOCK O_NONBLOCK
    cmp rax, 0
    TEXPECT 1

    mov rax, SYS_FCNTL
    xor rdi, rdi
    mov rsi, F_GETFL
    xor rdx, rdx
    syscall
    and rax, O_NONBLOCK
    cmp rax, O_NONBLOCK
    TEXPECT 2

    mov rax, SYS_FCNTL
    xor rdi, rdi
    mov rsi, F_GETFD
    xor rdx, rdx
    syscall
    cmp rax, 0
    TEXPECT 3

    TREAD0
    cmp rax, EAGAIN
    TEXPECT 4

    TNONBLOCK 0
    mov qword ptr [TTY_READY], 1
    TREADN 16
    cmp rax, 4
    TEXPECT 5

    mov eax, [rip + ttybuf]
    cmp eax, 0x0a796568
    TEXPECT 6

    TNONBLOCK O_NONBLOCK
    mov qword ptr [TTY_READY2], 1
    mov r13, 400
.wait_go2:
    cmp qword ptr [TTY_GO2], 1
    je .got_go2
    mov rax, SYS_SLEEP
    mov rdi, 5
    syscall
    dec r13
    jnz .wait_go2
.got_go2:
    TREAD0
    cmp rax, EAGAIN
    TEXPECT 7

    TNONBLOCK 0
    mov qword ptr [TTY_READY3], 1
    TREADN 16
    cmp rax, 3
    TEXPECT 8

    mov eax, [rip + ttybuf]
    and eax, 0x00ffffff
    cmp eax, 0x000a6361
    TEXPECT 9

    TREADN 2
    cmp rax, 2
    TEXPECT 10

    TREADN 16
    cmp rax, 3
    TEXPECT 11

    mov qword ptr [TTY_READY4], 1
    mov rax, SYS_SLEEP
    mov rdi, 150
    syscall
    cmp rax, 0
    TEXPECT 12

    mov qword ptr [TTY_DONE], 1
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

tty_report:
    mov qword ptr [TTY_DONE], 2
    mov rax, SYS_EXIT
    mov rdi, 1
    syscall

    .align 8
ttybuf:
    .space 16

.global user_test_end
.type user_test_end,@object
.align 8
user_test_end:
