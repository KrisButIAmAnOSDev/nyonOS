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

static void ck_true(int cond) {
    if (cond) ok();
    else bad(0, 1);
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

#define WFILE "/WTFILE.TXT"
#define WOUT  "/FSOUT.BIN"
#define WDIR  "/WTDIR"
#define CHUNK 1024
#define REPS  20

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;

    char buf[CHUNK];
    struct sys_stat st;

    int fd = (int)sys_open(WFILE, O_WRONLY | O_CREAT);
    ck_true(fd >= 0);
    ck(sys_write(fd, (void *)"hello world", 11), 11);
    ck(sys_close(fd), 0);

    fd = (int)sys_open(WFILE, O_RDONLY);
    ck_true(fd >= 0);
    wipe(buf, 64);
    ck(sys_read(fd, buf, 11), 11);
    ck_true(streq(buf, "hello world"));
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 11);
    ck(sys_close(fd), 0);

    fd = (int)sys_open(WFILE, O_WRONLY | O_TRUNC);
    ck_true(fd >= 0);
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 0);
    ck(sys_write(fd, (void *)"abc", 3), 3);
    ck(sys_close(fd), 0);

    fd = (int)sys_open(WFILE, O_WRONLY | O_APPEND);
    ck_true(fd >= 0);
    ck(sys_write(fd, (void *)"def", 3), 3);
    ck(sys_close(fd), 0);

    fd = (int)sys_open(WFILE, O_RDONLY);
    ck_true(fd >= 0);
    wipe(buf, 64);
    ck(sys_read(fd, buf, 6), 6);
    ck_true(streq(buf, "abcdef"));
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 6);
    ck(sys_close(fd), 0);

    for (int i = 0; i < CHUNK; i++) buf[i] = (char)(i & 0x7f);

    fd = (int)sys_open(WOUT, O_WRONLY | O_CREAT | O_TRUNC);
    ck_true(fd >= 0);
    for (int r = 0; r < REPS; r++) ck(sys_write(fd, buf, CHUNK), CHUNK);
    ck(sys_fsync(fd), 0);
    ck(sys_close(fd), 0);

    fd = (int)sys_open(WOUT, O_RDONLY);
    ck_true(fd >= 0);
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, CHUNK * REPS);
    wipe(buf, CHUNK);
    ck(sys_read(fd, buf, CHUNK), CHUNK);
    int mismatch = 0;
    for (int i = 0; i < CHUNK; i++) if (buf[i] != (char)(i & 0x7f)) mismatch++;
    ck(mismatch, 0);
    ck(sys_lseek(fd, CHUNK * 3, 0), CHUNK * 3);
    wipe(buf, CHUNK);
    ck(sys_read(fd, buf, CHUNK), CHUNK);
    mismatch = 0;
    for (int i = 0; i < CHUNK; i++) if (buf[i] != (char)(i & 0x7f)) mismatch++;
    ck(mismatch, 0);
    ck(sys_close(fd), 0);

    ck(sys_mkdir(WDIR, 0755), 0);
    ck(sys_rmdir(WDIR), 0);
    ck(sys_rmdir(WDIR), ENOENT);

    ck(sys_unlinkat(AT_FDCWD, WFILE, 0), 0);
    ck(sys_open(WFILE, O_RDONLY), ENOENT);
    ck(sys_open("/NOPE.TXT", O_RDONLY), ENOENT);

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    sys_exit(failed ? 1 : 0);
    return 0;
}
