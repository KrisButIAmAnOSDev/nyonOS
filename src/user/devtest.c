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

int main(void) {
    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;

    slong fd = sys_open("/DEV/ZERO", O_RDONLY);
    ck_true(fd >= 0);
    if (fd >= 0) {
        uint8_t zbuf[64];
        for (int i = 0; i < 64; i++) zbuf[i] = 0xFF;
        ck(sys_read((int)fd, zbuf, 64), 64);
        int nonzero = 0;
        for (int i = 0; i < 64; i++) if (zbuf[i]) nonzero++;
        ck(nonzero, 0);
        ck(sys_close((int)fd), 0);
    }

    fd = sys_open("/DEV/NULL", O_WRONLY);
    ck_true(fd >= 0);
    if (fd >= 0) {
        ck(sys_write((int)fd, (void *)"hi", 2), 2);
        ck(sys_close((int)fd), 0);
    }

    fd = sys_open("/DEV/FB0", O_RDONLY);
    ck_true(fd >= 0);
    if (fd >= 0) {
        uint32_t px;
        ck(sys_read((int)fd, &px, 4), 4);
        ck(sys_close((int)fd), 0);
    }

    fd = sys_open("/DEV", O_RDONLY | O_DIRECTORY);
    ck_true(fd >= 0);
    if (fd >= 0) {
        struct sys_dirent de;
        int found_input = 0;
        int entries = 0;
        for (unsigned i = 0; i < 16; i++) {
            slong r = sys_getdents((int)fd, i, &de);
            if (r <= 0) break;
            entries++;
            if (de.name[0] == 'I' || de.name[0] == 'i') found_input = 1;
        }
        ck(entries, 7);
        ck_true(found_input);
        ck(sys_close((int)fd), 0);
    }

    fd = sys_open("/DEV/CONSOLE", O_RDWR);
    ck_true(fd >= 0);
    if (fd >= 0) {
        ck(sys_write((int)fd, (void *)"dev-test\n", 9), 9);
        ck(sys_close((int)fd), 0);
    }

    fd = sys_open("/DEV/TTY0", O_RDWR);
    ck_true(fd >= 0);
    if (fd >= 0) {
        ck(sys_close((int)fd), 0);
    }

    fd = sys_open("/DEV/TTY", O_RDWR);
    ck(fd, ENXIO);

    ck(sys_setsid(), sys_getpid());
    ck(sys_getpgid(0), sys_getpid());
    ck(sys_setpgid(0, 0), 0);
    ck(sys_getpgid(0), sys_getpid());
    ck(sys_kill(-(slong)sys_getpid(), 0), 0);
    ck(sys_kill(-99999, 0), ESRCH);

    int fg = 0;
    ck(sys_ioctl(2, KTTY_SETFG, (long)&fg), 0);
    ck_true(fg == 0);

    slong me = sys_getpid();
    ck(sys_ioctl(2, KTTY_SETFG, (long)&me), EIO);

    uint64_t ttou = 1ULL << (22 - 1);
    ck(sys_sigprocmask(0, &ttou, 0), 0);
    ck(sys_ioctl(2, KTTY_SETFG, (long)&me), 0);
    uint64_t none = 0;
    ck(sys_sigprocmask(2, &none, 0), 0);
    ck(sys_ioctl(2, KTTY_SETFG, (long)&fg), 0);

    ck(sys_ioctl(2, KTTY_SETCTTY, 0), 0);
    fg = 0;
    ck(sys_ioctl(2, KTTY_GETFG, (long)&fg), 0);
    ck_true(fg == sys_getpid());

    fd = sys_open("/DEV/TTY", O_RDWR);
    ck_true(fd >= 0);
    if (fd >= 0) {
        ck(sys_close((int)fd), 0);
    }

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
