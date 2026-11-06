#include "res.h"
#include <string.h>

static volatile ulong *res = (volatile ulong *)RES_VIRT;
static int idx;
static int failed;

static void ok(void) {
    res[RES_CHECK_BASE + idx] = 1;
    idx++;
}

static void bad(slong got, slong want) {
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

int main(void) {
    res_reset_pending(res);

    {
        int ef = sys_eventfd2(0, ABI_EFD_NONBLOCK);
        ck(ef >= 0, 1);
        uint64_t v = 0x1234;
        ck(sys_write(ef, &v, 8), 8);
        uint64_t r = 0;
        ck(sys_read(ef, &r, 8), 8);
        ck(r == 0x1234, 1);
        ck(sys_read(ef, &r, 8), -11);
        ck(sys_close(ef), 0);
    }

    {
        int se = sys_eventfd2(0, ABI_EFD_NONBLOCK | ABI_EFD_SEMAPHORE);
        ck(se >= 0, 1);
        uint64_t v = 1;
        ck(sys_write(se, &v, 8), 8);
        uint64_t r = 0;
        ck(sys_read(se, &r, 8), 8);
        ck(r == 1, 1);
        ck(sys_read(se, &r, 8), -11);
        ck(sys_close(se), 0);
    }

    {
        int mf = sys_memfd_create(0, ABI_MFD_CLOEXEC);
        ck(mf >= 0, 1);
        const char *pl = "hello memfd";
        size_t plen = strlen(pl) + 1;
        ck(sys_write(mf, pl, plen), (slong)plen);
        ck(sys_lseek(mf, 0, ABI_SEEK_SET), 0);
        char buf[32];
        memset(buf, 0, sizeof buf);
        ck(sys_read(mf, buf, sizeof buf), (slong)plen);
        ck(strcmp(buf, pl) == 0, 1);
        ck(sys_ftruncate(mf, 4), 0);
        memset(buf, 0, sizeof buf);
        ck(sys_read(mf, buf, sizeof buf), 0);
        ck(sys_lseek(mf, 0, ABI_SEEK_SET), 0);
        ck(sys_read(mf, buf, 8), 4);
        ck(buf[3] == pl[3], 1);
        ck(sys_close(mf), 0);
    }

    int tf = -1;
    int ep = -1;
    {
        tf = sys_timerfd_create(1, 0);
        ck(tf >= 0, 1);
        struct sys_itimerspec its;
        memset(&its, 0, sizeof its);
        its.it_value.nsec = 150 * 1000000LL;
        ck(sys_timerfd_settime(tf, 0, &its, 0), 0);
        struct sys_itimerspec got;
        memset(&got, 0, sizeof got);
        ck(sys_timerfd_gettime(tf, &got), 0);
        ck(got.it_value.sec == 0 && got.it_value.nsec > 0, 1);

        ep = sys_epoll_create1(0);
        ck(ep >= 0, 1);
        struct sys_epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events = ABI_EPOLLIN;
        ev.data = 0xAA;
        ck(sys_epoll_ctl(ep, ABI_EPOLL_CTL_ADD, tf, &ev), 0);
        struct sys_epoll_event outs[4];
        memset(outs, 0, sizeof outs);
        ck(sys_epoll_wait(ep, outs, 4, 0), 0);
        int n = sys_epoll_wait(ep, outs, 4, 2000);
        ck(n >= 1, 1);
        ck(outs[0].events & ABI_EPOLLIN, 1);
        ck(outs[0].data == 0xAA, 1);
        uint64_t exp = 0;
        ck(sys_read(tf, &exp, 8), 8);
        ck(exp >= 1, 1);

        struct sys_itimerspec per;
        memset(&per, 0, sizeof per);
        per.it_interval.nsec = 40 * 1000000LL;
        per.it_value.nsec = 40 * 1000000LL;
        ck(sys_timerfd_settime(tf, 0, &per, 0), 0);
        sys_sleep(300);
        exp = 0;
        ck(sys_read(tf, &exp, 8), 8);
        ck(exp >= 2, 1);
        ck(sys_close(tf), 0);
    }

    {
        int ur = sys_open("/dev/urandom", 0);
        ck(ur >= 0, 1);
        unsigned char a[16], b[16];
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        ck(sys_read(ur, a, sizeof a), 16);
        ck(sys_read(ur, b, sizeof b), 16);
        ck(memcmp(a, b, sizeof a) != 0, 1);
        ck(sys_close(ur), 0);

        int rr = sys_open("/dev/random", 0);
        ck(rr >= 0, 1);
        memset(b, 0, sizeof b);
        ck(sys_read(rr, b, sizeof b), 16);
        ck(memcmp(a, b, sizeof a) != 0, 1);
        ck(sys_close(rr), 0);
    }

    {
        uint64_t mask = 1ULL << (ABI_SIGUSR1 - 1);
        int sf = sys_signalfd(-1, &mask, 0);
        ck(sf >= 0, 1);
        ck(sys_kill(sys_getpid(), ABI_SIGUSR1), 0);
        struct sys_signalfd_siginfo si;
        memset(&si, 0, sizeof si);
        ck(sys_read(sf, &si, sizeof si), (slong)sizeof si);
        ck(si.ssi_signo == ABI_SIGUSR1, 1);
        ck(sys_kill(sys_getpid(), ABI_SIGUSR1), 0);
        memset(&si, 0, sizeof si);
        ck(sys_read(sf, &si, sizeof si), (slong)sizeof si);
        ck(si.ssi_signo == ABI_SIGUSR1, 1);
        ck(sys_close(sf), 0);

        uint64_t bad = 1ULL << (ABI_SIGKILL - 1);
        ck(sys_signalfd(-1, &bad, 0), -22);
        uint64_t good = 1ULL << (ABI_SIGUSR1 - 1);
        int sf2 = sys_signalfd(-1, &good, 0);
        ck(sf2 >= 0, 1);
        ck(sys_signalfd(sf2, &bad, 0), -22);
        ck(sys_close(sf2), 0);
    }

    {
        int fds[2];
        ck(sys_pipe(fds), 0);
        struct sys_epoll_event pev;
        memset(&pev, 0, sizeof pev);
        pev.events = ABI_EPOLLIN;
        pev.data = 0x77;
        ck(sys_epoll_ctl(ep, ABI_EPOLL_CTL_ADD, fds[0], &pev), 0);
        struct sys_epoll_event outs[4];
        memset(outs, 0, sizeof outs);
        ck(sys_epoll_wait(ep, outs, 4, 0), 0);
        const char *msg = "p";
        ck(sys_write(fds[1], msg, 1), 1);
        int n = sys_epoll_wait(ep, outs, 4, 500);
        ck(n >= 1, 1);
        ck(outs[0].data == 0x77, 1);
        char ch = 0;
        ck(sys_read(fds[0], &ch, 1), 1);
        ck(ch == 'p', 1);
        ck(sys_epoll_wait(ep, outs, 4, 0), 0);
        ck(sys_epoll_ctl(ep, ABI_EPOLL_CTL_DEL, fds[0], 0), 0);
        ck(sys_write(fds[1], msg, 1), 1);
        ck(sys_epoll_wait(ep, outs, 4, 0), 0);
        ck(sys_close(fds[0]), 0);
        ck(sys_close(fds[1]), 0);
        ck(sys_close(ep), 0);
    }

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
