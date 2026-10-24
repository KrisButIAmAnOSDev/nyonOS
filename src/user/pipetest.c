#include "res.h"

static volatile ulong *res = (volatile ulong *)RES_VIRT;
static int idx;
static int failed;

static void ok(void) {
    res[RES_CHECK_BASE + idx] = 1;
    idx++;
}

static void bad(slong got, slong want) {
    res[RES_CHECK_BASE + idx] = 0;
    res[RES_CHECK_BASE + idx] = 0;
    res[RES_FAIL_BASE + idx] = (ulong)got;
    res[RES_GOT_BASE + idx] = (ulong)want;
    idx++;
    failed++;
}

static void ck(slong got, slong want) {
    if (got == want) ok();
    else bad(got, want);
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void wipe(char *b, int n) {
    for (int i = 0; i < n; i++) b[i] = 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;

    char buf[256];
    int p[2];

    ck(sys_pipe(p), 0);
    ck(p[0] >= 0, 1);
    ck(p[1] >= 0, 1);
    ck(p[0] != p[1], 1);

    ck(sys_write(p[1], (void *)"hello", 5), 5);
    wipe(buf, 64);
    ck(sys_read(p[0], buf, 5), 5);
    ck(streq(buf, "hello"), 1);

    ck(sys_close(p[0]), 0);
    ck(sys_close(p[1]), 0);

    int e[2];
    ck(sys_pipe(e), 0);
    ck(sys_close(e[1]), 0);
    wipe(buf, 64);
    ck(sys_read(e[0], buf, 5), 0);
    ck(sys_close(e[0]), 0);

    int b[2];
    ck(sys_pipe(b), 0);
    ck(sys_close(b[0]), 0);
    ck(sys_write(b[1], (void *)"x", 1), EPIPE);
    ck(sys_close(b[1]), 0);

    int q[2];
    struct sys_pollfd pfd;
    ck(sys_pipe(q), 0);
    pfd.fd = q[0];
    pfd.events = ABI_POLLIN;
    pfd.revents = 0;
    ck(sys_poll(&pfd, 1, 0, 0), 0);
    ck(pfd.revents, 0);
    ck(sys_write(q[1], (void *)"z", 1), 1);
    pfd.revents = 0;
    ck(sys_poll(&pfd, 1, 0, 0), 1);
    ck(pfd.revents & ABI_POLLIN, ABI_POLLIN);
    pfd.fd = q[1];
    pfd.events = ABI_POLLOUT;
    pfd.revents = 0;
    ck(sys_poll(&pfd, 1, 0, 0), 1);
    ck(pfd.revents & ABI_POLLOUT, ABI_POLLOUT);
    ck(sys_close(q[0]), 0);
    ck(sys_close(q[1]), 0);

    uint32_t cpid = 0;
    uintptr_t status = 0;

    int f[2];
    ck(sys_pipe(f), 0);
    slong pid = sys_fork((uint32_t *)&cpid);
    if (pid == 0) {
        sys_write(f[1], (void *)"xyz", 3);
        sys_exit(0);
        for (;;) __asm__ volatile("hlt");
    }
    ck(pid > 0, 1);
    wipe(buf, 64);
    ck(sys_read(f[0], buf, 3), 3);
    ck(streq(buf, "xyz"), 1);
    status = 0;
    ck(sys_waitpid((int32_t)cpid, (uintptr_t)&status, 0), (slong)cpid);
    ck(status, 0);
    ck(sys_close(f[0]), 0);
    ck(sys_close(f[1]), 0);

    int g[2];
    ck(sys_pipe(g), 0);
    cpid = 0;
    pid = sys_fork((uint32_t *)&cpid);
    if (pid == 0) {
        char rb[256];
        int got = 0;
        while (got < ABI_PIPE_CAPACITY + 1) {
            slong r = sys_read(g[0], rb, 256);
            if (r <= 0) break;
            got += (int)r;
        }
        sys_exit(got == ABI_PIPE_CAPACITY + 1 ? 0 : 1);
        for (;;) __asm__ volatile("hlt");
    }
    ck(pid > 0, 1);
    ulong big = (ulong)sys_mmap(ABI_PIPE_CAPACITY,
                                ABI_MMAP_PROT_READ | ABI_MMAP_PROT_WRITE, ABI_MMAP_ANON);
    ck(big >= 0x100000000L, 1);
    char *wbuf = (char *)big;
    for (int i = 0; i < ABI_PIPE_CAPACITY; i++) wbuf[i] = (char)(i & 0x7f);
    ck(sys_write(g[1], wbuf, ABI_PIPE_CAPACITY), ABI_PIPE_CAPACITY);
    ck(sys_write(g[1], wbuf, 1), 1);
    ck(sys_munmap(big, ABI_PIPE_CAPACITY), 0);
    status = 0;
    ck(sys_waitpid((int32_t)cpid, (uintptr_t)&status, 0), (slong)cpid);
    ck(status, 0);
    ck(sys_close(g[0]), 0);
    ck(sys_close(g[1]), 0);

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    sys_exit(failed ? 1 : 0);
    return 0;
}
