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

    struct sys_input_event ev;

    slong fd = sys_open("/DEV/INPUT/EVENT0", O_RDONLY | O_NONBLOCK);
    ck_true(fd >= 0);
    if (fd < 0) {
        res[RES_DONE] = RES_FAILED;
        return 1;
    }

    int seen_a_down = 0, seen_a_up = 0, seen_up = 0;
    int waited = 0;
    for (; waited < 2000; waited++) {
        slong r = sys_read((int)fd, &ev, sizeof ev);
        if (r == EAGAIN) {
            sys_sleep(5);
            continue;
        }
        ck_true(r == (slong)sizeof ev);
        ck_true(ev.type == SYS_EV_KEY);
        if (ev.code == 30 && ev.value == 1) seen_a_down = 1;
        if (ev.code == 30 && ev.value == 0) seen_a_up = 1;
        if (ev.code == 103 && ev.value == 1) seen_up = 1;
        if (seen_a_down && seen_a_up && seen_up) break;
    }
    ck_true(seen_a_down);
    ck_true(seen_a_up);
    ck_true(seen_up);
    ck_true(waited < 2000);

    int drained = 0;
    for (; drained < 100; drained++) {
        if (sys_read((int)fd, &ev, sizeof ev) == EAGAIN) break;
        sys_sleep(5);
    }
    ck_true(drained < 100);

    ck(sys_close((int)fd), 0);

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
