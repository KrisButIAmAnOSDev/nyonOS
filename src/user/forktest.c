#include "res.h"

#define SYS_EFAULT (-14)
#define SYS_ECHILD (-10)
#define SYS_EINVAL (-22)

#define WNOHANG ABI_WNOHANG

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

static void ck_true(int v) {
    ck(v, 1);
}

static ulong marker;

static void child_touch_and_exit(int code) {
    marker = 0xfeedfacecafe0000UL;
    sys_exit(code);
}

static void child_vfork_touch(void) {
    marker = 0xfeedfacecafe0000UL;
    sys_exit(0);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;

    uint32_t cpid = 0;
    int32_t status = 0;
    marker = 0;

    slong r = sys_fork((uint32_t *)&cpid);
    if (r == 0) {
        child_touch_and_exit(11);
        for (;;) __asm__ volatile("hlt");
    }

    ck_true(r > 0);
    ck_true(cpid > 0);
    ck_true((slong)cpid == r);
    ck_true((slong)sys_getpid() != (slong)cpid);
    ck_true(marker == 0);

    status = 0;
    ck(sys_waitpid((int32_t)cpid, (uintptr_t)&status, 0), (slong)cpid);
    ck(status, 11 << 8);
    ck_true(marker == 0);

    marker = 0;
    cpid = 0;

    r = sys_clone(0, 0, (uintptr_t)&cpid, 0);
    if (r == 0) {
        child_touch_and_exit(22);
        for (;;) __asm__ volatile("hlt");
    }

    ck_true(r > 0);
    ck_true((slong)cpid == r);
    ck_true(marker == 0);

    status = 0;
    ck(sys_waitpid(-1, (uintptr_t)&status, 0), (slong)cpid);
    ck(status, 22 << 8);

    marker = 0;
    cpid = 0;

    if (sys_vfork((uint32_t *)&cpid) == 0) {
        child_vfork_touch();
        for (;;) __asm__ volatile("hlt");
    }

    ck_true(cpid > 0);
    ck_true((slong)cpid != (slong)sys_getpid());
    ck_true(marker == 0xfeedfacecafe0000UL);

    status = 0;
    ck(sys_waitpid((int32_t)cpid, (uintptr_t)&status, 0), (slong)cpid);
    ck(status, 0);

    marker = 0x1234;
    cpid = 0;

    if (sys_clone(ABI_CLONE_VM | ABI_CLONE_FILES | ABI_CLONE_SIGHAND,
                  0, (uintptr_t)&cpid, 0) == 0) {
        child_touch_and_exit(33);
        for (;;) __asm__ volatile("hlt");
    }

    ck_true(cpid > 0);
    ck_true((slong)cpid != (slong)sys_getpid());
    ck_true(marker == 0xfeedfacecafe0000UL);

    status = 0;
    ck(sys_waitpid((int32_t)cpid, (uintptr_t)&status, 0), (slong)cpid);
    ck(status, 33 << 8);

    ck(sys_waitpid(-1, 0, WNOHANG), SYS_ECHILD);
    ck(sys_waitpid(123456, 0, WNOHANG), SYS_ECHILD);
    ck(sys_waitpid(-7, 0, 0), SYS_EINVAL);

    ck(sys_fork(0), SYS_EFAULT);
    ck(sys_vfork(0), SYS_EFAULT);

    ck_true(sys_getpid() > 0);

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    sys_exit(failed ? 1 : 0);
    return 0;
}