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

    struct sys_fb fb;
    ck(sys_fb_map(&fb), 0);
    ck_true(fb.width > 0);
    ck_true(fb.height > 0);
    ck_true(fb.pitch >= fb.width * 4);
    ck_true(fb.phys != 0);

    volatile uint32_t *fbuf = (volatile uint32_t *)(uintptr_t)fb.phys;

    uint32_t midx = fb.width / 2;
    uint32_t midy = fb.height / 2;
    uint32_t old = fbuf[midy * (fb.pitch / 4) + midx];

    fbuf[midy * (fb.pitch / 4) + midx] = 0x00FF00FF;
    ck((slong)fbuf[midy * (fb.pitch / 4) + midx], 0x00FF00FF);

    fbuf[midy * (fb.pitch / 4) + midx] = 0xFEDCBA98;
    ck((slong)fbuf[midy * (fb.pitch / 4) + midx], 0xFEDCBA98);

    fbuf[midy * (fb.pitch / 4) + midx] = old;

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
