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

static void ck_ge(slong got, slong min) {
    if (got >= min) ok();
    else bad(got, min);
}

#define PAGE 4096UL

#define FX_READY 464
#define FX_WAITER 466
#define FX_WAKER 468
#define FX_WORDEVEN 470

static int streq(char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void futex_role(int argc, char **argv) {
    if (argc < 2) return;
    volatile uint32_t *word = (volatile uint32_t *)&res[FX_WORDEVEN];

    if (streq(argv[1], "waker")) {
        for (int i = 0; i < 400 && res[FX_READY] != 0x5eed; i++) sys_sleep(1);
        __sync_synchronize();
        *word = 99;
        for (int i = 0; i < 400 && res[FX_WAITER] == 0; i++) {
            sys_futex((uint32_t *)word, ABI_FUTEX_WAKE, 0);
            sys_sleep(1);
        }
        res[FX_WAKER] = 100;
        sys_exit(0);
    }

    if (streq(argv[1], "waiter")) {
        *word = 42;
        __sync_synchronize();
        res[FX_READY] = 0x5eed;
        slong r = sys_futex((uint32_t *)word, ABI_FUTEX_WAIT, 42);
        res[FX_WAITER] = (ulong)(r + 100);
        sys_exit(0);
    }
}

int main(int argc, char **argv) {
    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;

    if (argc >= 2) {
        futex_role(argc, argv);
        sys_exit(2);
    }

    slong m = sys_mmap(PAGE, ABI_MMAP_PROT_READ | ABI_MMAP_PROT_WRITE, ABI_MMAP_ANON);
    ck_ge(m, 0x100000000L);
    ck(m & (PAGE - 1), 0);

    volatile uint64_t *p = (volatile uint64_t *)(ulong)m;
    ck((slong)p[0], 0);
    p[0] = 0x1122334455667788UL;
    ck((slong)p[0], (slong)0x1122334455667788UL);

    slong m2 = sys_mmap(3 * PAGE, ABI_MMAP_PROT_READ | ABI_MMAP_PROT_WRITE, ABI_MMAP_ANON);
    ck_ge(m2, 0x100000000L);
    ck(m2 != m, 1);
    ck(m2 >= m + (slong)PAGE || m2 + (slong)(3 * PAGE) <= m, 1);

    ck(sys_mmap(0, ABI_MMAP_PROT_READ, ABI_MMAP_ANON), EINVAL);
    ck(sys_mmap(PAGE, ABI_MMAP_PROT_READ, 0), EINVAL);
    ck(sys_mmap(PAGE, 0x80, ABI_MMAP_ANON), EINVAL);

    ck(sys_munmap((ulong)m, PAGE), 0);
    ck(sys_munmap((ulong)m, PAGE), EINVAL);
    ck(sys_munmap((ulong)m2 + 1, PAGE), EINVAL);
    ck(sys_munmap((ulong)m2, 3 * PAGE), 0);

    slong m3 = sys_mmap(PAGE, ABI_MMAP_PROT_READ | ABI_MMAP_PROT_WRITE, ABI_MMAP_ANON);
    ck_ge(m3, 0x100000000L);
    ck(sys_munmap((ulong)m3, PAGE), 0);

    struct sys_timespec t1, t2;
    ck(sys_clock_gettime(ABI_CLOCK_MONOTONIC, &t1), 0);
    sys_sleep(20);
    ck(sys_clock_gettime(ABI_CLOCK_MONOTONIC, &t2), 0);
    ck((t2.sec * 1000000000L + t2.nsec) - (t1.sec * 1000000000L + t1.nsec) >= 20000000L, 1);
    ck(sys_clock_gettime(ABI_CLOCK_REALTIME, &t1), ENOSYS);
    ck(sys_clock_gettime(ABI_CLOCK_MONOTONIC, (struct sys_timespec *)0), EFAULT);

    volatile uint32_t word = 7;
    ck(sys_futex((uint32_t *)&word, ABI_FUTEX_WAIT, 9), EAGAIN);
    ck(sys_futex((uint32_t *)&word, ABI_FUTEX_WAKE, 0), 0);
    ck(sys_futex((uint32_t *)0, ABI_FUTEX_WAKE, 0), EINVAL);
    ck(sys_futex((uint32_t *)((ulong)&word + 1), ABI_FUTEX_WAKE, 0), EINVAL);

    ck(sys_set_fs_base(0x200000UL), 0);
    ck(sys_set_fs_base(0xFFFF800000000000UL), EINVAL);
    ck(sys_set_fs_base(0), 0);

    uint64_t *sp = (uint64_t *)argv - 1;
    uint64_t *env = sp + 1 + argc + 1;
    while (*env) env++;
    uint64_t *au = env + 1;

    int saw_pagesz = 0, saw_entry = 0, saw_phnum = 0, saw_phent = 0, saw_random = 0, saw_phdr = 0;
    for (int i = 0; au[i * 2] != 0 && i < 16; i++) {
        uint64_t t = au[i * 2], v = au[i * 2 + 1];
        if (t == ABI_AT_PAGESZ) saw_pagesz = (v == PAGE);
        else if (t == ABI_AT_ENTRY) saw_entry = (v >= 0x400000UL);
        else if (t == ABI_AT_PHNUM) saw_phnum = (v > 0);
        else if (t == ABI_AT_PHENT) saw_phent = (v == 56);
        else if (t == ABI_AT_RANDOM) saw_random = (v != 0);
        else if (t == ABI_AT_PHDR) saw_phdr = (v != 0);
    }
    ck(saw_pagesz, 1);
    ck(saw_entry, 1);
    ck(saw_phnum, 1);
    ck(saw_phent, 1);
    ck(saw_random, 1);
    ck(saw_phdr, 1);

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    sys_exit(failed ? 1 : 0);
    return 0;
}
