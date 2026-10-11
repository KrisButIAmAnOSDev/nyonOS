#include "res.h"

static volatile ulong *res = (volatile ulong *)RES_VIRT;
static int idx;
static int failed;

static void ok(void) {
    res[RES_CHECK_BASE + idx] = 1;
    idx++;
}

static void bad(slong got, slong want) {
    res[RES_FAIL_BASE + idx] = (ulong)got;
    res[RES_GOT_BASE + idx] = (ulong)want;
    idx++;
    failed++;
}

static void ck(slong got, slong want) {
    if (got == want) ok();
    else bad(got, want);
}

static ulong abi_want[8];
static ulong abi_got[8];
struct abi_area {
    struct sys_stat st;
    char path[16];
};
static struct abi_area abi_area;

static slong abi_probe(int use_int80) {
    abi_want[0] = 0x1111111111111111UL;
    abi_want[1] = 0x3333333333333333UL;
    abi_want[2] = 0x4444444444444444UL;
    abi_want[3] = 0x5555555555555555UL;
    abi_want[4] = 0x6666666666666666UL;
    abi_want[5] = 0x7777777777777777UL;
    abi_want[6] = 0x8888888888888888UL;
    abi_area.st.size = 0;
    const char *src = "/TEST.TXT";
    for (int i = 0; i < 10; i++) abi_area.path[i] = src[i];

    ulong rc = 0;

    __asm__ volatile(
        "push %%rbx\n\tpush %%r12\n\tpush %%r13\n\tpush %%r14\n\tpush %%r15\n\t"
        "mov %%rsp, %[g7]\n\t"
        "mov 0(%[w]), %%rbx\n\tmov 8(%[w]), %%r12\n\tmov 16(%[w]), %%r13\n\t"
        "mov 24(%[w]), %%r14\n\tmov 32(%[w]), %%r15\n\t"
        "mov 40(%[w]), %%rdi\n\tmov 48(%[w]), %%rsi\n\t"

        "mov $18, %%eax\n\tmov $1, %%edi\n\t"
        "test %[mode], %[mode]\n\tjz 1f\n\t"
        ".byte 0xcd, 0x80\n\tjmp 2f\n"
        "1:\n\tsyscall\n"
        "2:\n\ttest %%rax, %%rax\n\tjne 3f\n\t"

        "mov $11, %%eax\n\tmov %[a], %%rsi\n\tlea 56(%[a]), %%rdi\n\t"
        "test %[mode], %[mode]\n\tjz 4f\n\t"
        ".byte 0xcd, 0x80\n\tjmp 5f\n"
        "4:\n\tsyscall\n"
        "5:\n\ttest %%rax, %%rax\n\tjne 7f\n\t"

        "mov %%rbx, %[g0]\n\tmov %%r12, %[g1]\n\tmov %%r13, %[g2]\n\t"
        "mov %%r14, %[g3]\n\tmov %%r15, %[g4]\n\tmov %%rdi, %[g5]\n\t"
        "mov %%rsi, %[g6]\n\t"
        "mov %[g7], %%rax\n\tsub %%rsp, %%rax\n\tmov %%rax, %[g7]\n\t"
        "xor %%eax, %%eax\n\tjmp 6f\n"
        "3:\n\tmov $1, %%eax\n\tjmp 6f\n"
        "7:\n\tmov $2, %%eax\n"
        "6:\n\t"
        "pop %%r15\n\tpop %%r14\n\tpop %%r13\n\tpop %%r12\n\tpop %%rbx\n\t"
        "mov %%rax, %[out]\n\t"
        : [out] "=&r"(rc), [g0] "=&m"(abi_got[0]), [g1] "=&m"(abi_got[1]),
          [g2] "=&m"(abi_got[2]), [g3] "=&m"(abi_got[3]), [g4] "=&m"(abi_got[4]),
          [g5] "=&m"(abi_got[5]), [g6] "=&m"(abi_got[6]), [g7] "=&m"(abi_got[7])
        : [mode] "r"((ulong)use_int80), [w] "r"((ulong)abi_want),
          [a] "r"((ulong)&abi_area)
        : "rax", "rbx", "rdi", "rsi", "r10", "r11", "r12", "r13", "r14", "r15",
          "rcx", "memory");


    if (rc) return 100 + (slong)rc;
    if (abi_area.st.size != 3000) return 102;
    for (int i = 0; i < 5; i++) if (abi_got[i] != abi_want[i]) return 110 + i;
    if (abi_got[7]) return 120 + (slong)abi_got[7];
    return 0;
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static char bss_probe[64];
static ulong data_probe = 0x5a5a5a5a5a5a5a5aUL;
static const char rodata_probe[] = "nyon from a loaded C program";

static void ask_stdin(int line) {
    res[RES_STDIN_REQ] = (ulong)line;
    for (int i = 0; i < 4000 && res[RES_STDIN_ACK] != (ulong)line; i++) sys_sleep(1);
}

int main(void) {
    char buf[256];
    struct sys_stat st;
    struct sys_dirent dent;

    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;

    slong f = sys_open("/TEST.TXT", O_RDONLY);
    ck(f >= 0, 1);
    if (f < 0) goto done;
    int fd = (int)f;

    ck(sys_read(fd, buf, 14), 14);
    buf[14] = 0;
    ck(streq(buf, "that my jarona"), 1);

    ck(sys_pread(fd, buf, 14, 0), 14);
    ck(streq(buf, "that my jarona"), 1);

    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 3000);
    ck(sys_stat("/TEST.TXT", &st), 0);
    ck((slong)st.size, 3000);

    ck(sys_isatty(fd), 0);
    ck(sys_isatty(1), 1);

    slong d = sys_open("/SUB", O_RDONLY | O_DIRECTORY);
    ck(d >= 0, 1);
    if (d >= 0) {
        ck(sys_getdents((int)d, 0, &dent), 1);
        ck((slong)dent.size, 14);
        ck(sys_getdents((int)d, 99, &dent), 0);
        sys_close((int)d);
    }

    ck(sys_getpid() != 0, 1);
    ck(sys_sleep(1), 0);
    ck(sys_debug_print("x", 1), EPERM);

    ck(sys_dup2(1, 5, 0), 5);
    ck(sys_write(5, "5", 1), 1);
    ck(sys_isatty(5), 1);
    ck(sys_dup2(99, 6, 0), EBADF);
    ck(sys_dup2(1, 1, 0), 1);
    ck(sys_fcntl(5, F_GETFD, 0), 1);
    sys_fcntl(5, F_SETFD, 0);
    ck(sys_fcntl(5, F_GETFD, 0), 0);

    ck(sys_ioctl(5, KTTY_GETDEST, 0), 2);
    sys_ioctl(5, KTTY_SETDEST, 0);
    ck(sys_ioctl(5, KTTY_GETDEST, 0), 0);
    sys_ioctl(5, KTTY_SETDEST, 2);
    ck(sys_ioctl(5, KTTY_GETDEST, 0), 2);

    sys_ioctl(1, KTTY_SETDEST, PRINT_NONE);

    ck(sys_read(1, buf, 4), EBADF);
    ck(sys_fcntl(1, F_GETFL, 0) != 0, 1);
    slong hi = sys_fcntl(1, F_DUPFD, 20);
    ck(hi >= 20, 1);
    if (hi >= 20) ck(sys_close((int)hi), 0);
    ck(sys_ioctl(99, KTTY_GETDEST, 0), EBADF);
    ck(sys_read(2, buf, 4), EBADF);
    ck(sys_write(0, "x", 1), EBADF);
    ck(sys_isatty(2), 1);

    ck(sys_dup2(1, 40, 0), 40);
    ck(sys_write(40, "x", 1), 1);
    ck(sys_close(40), 0);

    ck(sys_lseek(fd, 0, SEEK_SET), 0);
    ck(sys_read(fd, buf, 4), 4);
    ck(sys_lseek(fd, 999999, SEEK_SET), 3000);
    ck(sys_read(fd, buf, 4), 0);

    slong nr = sys_dup(fd, 0, FD_RIGHT_READ);
    ck(nr >= 0, 1);
    if (nr >= 0) {
        ck(sys_write((int)nr, "x", 1), EBADF);
        ck(sys_close((int)nr), 0);
    }

    ck(sys_close(fd), 0);
    ck(sys_read(fd, buf, 4), EBADF);
    ck(sys_close(fd), EBADF);
    ck(sys_open("/NOPE.TXT", O_RDONLY), ENOENT);
    ck(sys_open("/SUB", O_RDONLY), EISDIR);
    ck(sys_read(0, (void *)0xffffffff80000000UL, 8), EFAULT);
    ck(sys_write(1, "1", 1), 1);

    slong a = sys_getpid();
    slong b;
    __asm__ volatile(".byte 0xcd, 0x80" : "=a"(b) : "a"((ulong)SYS_getpid) : "memory");
    ck(a, b);

    slong w;
    __asm__ volatile(".byte 0xcd, 0x80"
                     : "=a"(w)
                     : "a"((ulong)SYS_write), "D"(1UL), "S"((ulong)"x"), "d"(1UL)
                     : "memory");
    ck(w, 1);

    ck(abi_probe(0), 0);
    ck(abi_probe(1), 0);

    ulong before[2] = { 0, 0 }, after[2] = { 0, 0 };
    __asm__ volatile("mov %%rcx, %0; mov %%r11, %1" : "=&m"(before[0]), "=&m"(before[1]));
    slong z = sys_raw(200);
    __asm__ volatile("mov %%rcx, %0; mov %%r11, %1" : "=&m"(after[0]), "=&m"(after[1]));
    ck(z, ENOSYS);
    ck(after[0] != before[0], 1);
    ck((slong)((after[1] >> 9) & 1), 1);

    ck((slong)bss_probe[0], 0);
    ck((slong)bss_probe[63], 0);
    ck((slong)data_probe, (slong)0x5a5a5a5a5a5a5a5aUL);
    ck(streq(rodata_probe, "nyon from a loaded C program"), 1);
    bss_probe[0] = 7;
    ck(bss_probe[0], 7);

    ck(sys_isatty(0), 1);
    ck(sys_fcntl(0, F_SETFL, O_NONBLOCK), 0);
    ck(sys_fcntl(0, F_GETFL, 0) & O_NONBLOCK, O_NONBLOCK);
    ck(sys_read(0, buf, 8), EAGAIN);
    ck(sys_fcntl(0, F_SETFL, 0), 0);
    ck(sys_fcntl(0, F_GETFL, 0) & O_NONBLOCK, 0);

    ask_stdin(1);
    ck(sys_ioctl(0, KTTY_GETFG, 0), sys_getpid());
    ck(sys_read(0, buf, 64), 16);
    buf[16] = 0;
    ck(streq(buf, "nyon from stdin\n"), 1);

    ask_stdin(4);
    ck(sys_read(0, buf, 64), 3);
    buf[3] = 0;
    ck(streq(buf, "xy\n"), 1);

    ask_stdin(3);
    ck(sys_read(0, buf, 4), 4);
    buf[4] = 0;
    ck(streq(buf, "abcd"), 1);
    ck(sys_read(0, buf, 64), 7);
    buf[7] = 0;
    ck(streq(buf, "efghij\n"), 1);

    ask_stdin(5);
    ck(sys_read(0, buf, 64), 9);
    buf[9] = 0;
    ck(streq(buf, "bacspace\n"), 1);

    sys_ioctl(1, KTTY_SETDEST, 2);
    ck(sys_ioctl(1, KTTY_GETDEST, 0), 2);
    ck(sys_ioctl(2, KTTY_GETDEST, 0), 0);
    sys_ioctl(2, KTTY_SETDEST, 1);
    ck(sys_ioctl(2, KTTY_GETDEST, 0), 1);
    ck(sys_ioctl(1, KTTY_GETDEST, 0), 2);
    sys_ioctl(2, KTTY_SETDEST, 0);

    ck(sys_write(2, "2", 1), 1);
    ck(sys_dup2(1, 60, 0), 60);
    ck(sys_write(60, "3", 1), 1);
    ck(sys_ioctl(60, KTTY_GETDEST, 0), 2);
    ck(sys_close(60), 0);

    ck(sys_dup2(2, 61, 0), 61);
    ck(sys_ioctl(61, KTTY_GETDEST, 0), 0);
    ck(sys_write(61, "4", 1), 1);
    ck(sys_close(61), 0);

    ck(sys_ioctl(0, KTTY_SETDEST, 0), EBADF);
    ck(sys_write(0, "x", 1), EBADF);
    ck(sys_read(0, (void *)0, 4), EFAULT);
    ck(sys_write(2, (void *)0xffffffff80000000UL, 4), EFAULT);

done:
    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    sys_exit(failed ? 1 : 0);
    return 0;
}
