#include "res.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sched.h>

static volatile ulong *res = (volatile ulong *)RES_VIRT;
static int idx;
static int failed;

static void ok(void) {
    res[RES_CHECK_BASE + idx] = 1;
    idx++;
}

static void bad(ulong got, ulong want) {
    res[RES_CHECK_BASE + idx] = 0;
    res[RES_FAIL_BASE + idx] = got;
    res[RES_GOT_BASE + idx] = want;
    idx++;
    failed++;
}

static void ck(slong got, slong want) {
    if (got == want) ok();
    else bad((ulong)got, (ulong)want);
}

static void ck_true(int v) {
    ck(v, 1);
}

static volatile int got_hup;
static volatile int got_term;

static void on_hup(int s) {
    (void)s;
    got_hup = 1;
}

static void on_term(int s) {
    (void)s;
    got_term = 1;
}

static int test_handler_delivery(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    memset(&old, 0, sizeof old);
    sa.sa_handler = on_hup;
    sigemptyset(&sa.sa_mask);
    ck(sigaction(SIGHUP, &sa, &old), 0);
    ck(old.sa_handler == SIG_DFL || old.sa_handler == SIG_IGN, 1);
    ck(sigaction(SIGHUP, 0, &old), 0);
    ck(old.sa_handler == on_hup, 1);
    ck(kill(getpid(), SIGHUP), 0);
    ck_true(got_hup == 1);
    return 0;
}

static int test_mask_blocks(void) {
    struct sigaction sa;
    sigset_t set, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    ck(sigaction(SIGTERM, &sa, 0), 0);
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    ck(sigprocmask(SIG_BLOCK, &set, &old), 0);
    ck(kill(getpid(), SIGTERM), 0);
    ck_true(got_term == 0);
    ck(sigprocmask(SIG_UNBLOCK, &set, 0), 0);
    ck_true(got_term == 1);
    return 0;
}

static int test_default_kills(void) {
    pid_t p = fork();
    if (p == 0) {
        for (;;) sched_yield();
    }
    ck(kill(p, SIGKILL), 0);
    int st = 0;
    ck(waitpid(p, &st, 0), p);
    ck_true(WIFSIGNALED(st));
    ck_true(WTERMSIG(st) == SIGKILL);
    return 0;
}

static int test_kill_invalid(void) {
    ck(kill(1, 999), -1);
    ck(errno == EINVAL, 1);
    ck(kill(99999999, 0), -1);
    ck(errno == ESRCH, 1);
    return 0;
}

int main(void) {
    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;
    test_handler_delivery();
    test_mask_blocks();
    test_default_kills();
    test_kill_invalid();
    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
