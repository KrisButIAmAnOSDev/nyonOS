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

#define HFILE "/HOLE.TXT"
#define MFILE "/MULTI.BIN"
#define DDIR  "/FDDIR"
#define DSUB  "/FDDIR/SUB"
#define DFILE "/FDDIR/SUB/NEST.TXT"

static char buf[4096];

static void hole_test(void) {
    int fd = (int)sys_open(HFILE, O_WRONLY | O_CREAT | O_TRUNC);
    ck_true(fd >= 0);
    if (fd < 0) return;
    ck(sys_write(fd, (void *)"start123", 8), 8);
    ck(sys_lseek(fd, 1000, SEEK_SET), 1000);
    ck(sys_write(fd, (void *)"end45678", 8), 8);
    struct sys_stat st;
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 1008);
    ck(sys_close(fd), 0);

    fd = (int)sys_open(HFILE, O_RDONLY);
    ck_true(fd >= 0);
    if (fd < 0) return;
    for (int i = 0; i < 8; i++) buf[i] = 0;
    ck(sys_read(fd, buf, 8), 8);
    ck_true(streq(buf, "start123"));
    ck(sys_lseek(fd, 1000, SEEK_SET), 1000);
    for (int i = 0; i < 8; i++) buf[i] = 0;
    ck(sys_read(fd, buf, 8), 8);
    ck_true(streq(buf, "end45678"));
    ck(sys_lseek(fd, 100, SEEK_SET), 100);
    for (int i = 0; i < 8; i++) buf[i] = 0x7f;
    ck(sys_read(fd, buf, 8), 8);
    int zero = 1;
    for (int i = 0; i < 8; i++) if (buf[i] != 0) zero = 0;
    ck_true(zero);
    ck(sys_close(fd), 0);
}

static void trunc_test(void) {
    struct sys_stat st;
    int fd = (int)sys_open(HFILE, O_RDWR);
    ck_true(fd >= 0);
    if (fd < 0) return;
    ck(sys_ftruncate(fd, 2000), 0);
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 2000);
    ck(sys_ftruncate(fd, 100), 0);
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 100);
    ck(sys_lseek(fd, 96, SEEK_SET), 96);
    ck(sys_read(fd, buf, 8), 4);
    ck(sys_ftruncate(fd, 0), 0);
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, 0);
    ck(sys_close(fd), 0);
    ck(sys_unlinkat(AT_FDCWD, HFILE, 0), 0);
}

static void multi_test(void) {
    struct sys_stat st;
    for (int i = 0; i < 4096; i++) buf[i] = (char)((i * 7 + 3) & 0xff);
    int fd = (int)sys_open(MFILE, O_WRONLY | O_CREAT | O_TRUNC);
    ck_true(fd >= 0);
    if (fd < 0) return;
    int total = 0;
    for (int r = 0; r < 5; r++) {
        ck(sys_write(fd, buf, 4096), 4096);
        total += 4096;
    }
    ck(sys_fsync(fd), 0);
    ck(sys_close(fd), 0);

    fd = (int)sys_open(MFILE, O_RDONLY);
    ck_true(fd >= 0);
    if (fd < 0) return;
    ck(sys_fstat(fd, &st), 0);
    ck((slong)st.size, total);
    for (int r = 0; r < 5; r++) {
        for (int i = 0; i < 4096; i++) buf[i] = 0;
        ck(sys_read(fd, buf, 4096), 4096);
        int badn = 0;
        for (int i = 0; i < 4096; i++) {
            int want = (i * 7 + 3) & 0xff;
            if ((unsigned char)buf[i] != (unsigned char)want) badn++;
        }
        ck(badn, 0);
    }
    ck(sys_close(fd), 0);
    ck(sys_unlinkat(AT_FDCWD, MFILE, 0), 0);
}

static void name_test(void) {
    char longname[512];
    longname[0] = '/';
    for (int i = 1; i < 500; i++) longname[i] = 'A' + (i % 26);
    longname[500] = 0;
    ck(sys_open(longname, O_RDONLY), ENAMETOOLONG);
    ck(sys_mkdir(longname, 0755), ENAMETOOLONG);
    ck(sys_unlinkat(AT_FDCWD, longname, 0), ENAMETOOLONG);
}

static void dir_test(void) {
    ck(sys_mkdir(DDIR, 0755), 0);
    ck(sys_mkdir(DDIR, 0755), EEXIST);
    ck(sys_mkdir(DSUB, 0755), 0);
    ck(sys_open(DSUB, O_WRONLY | O_DIRECTORY), EISDIR);
    ck(sys_unlinkat(AT_FDCWD, DSUB, 0), EISDIR);
    ck(sys_rmdir("/TEST.TXT"), ENOTDIR);
    ck(sys_mkdir("/TEST.TXT", 0755), EEXIST);
    ck(sys_rmdir(DDIR), ENOTEMPTY);

    int fd = (int)sys_open(DFILE, O_WRONLY | O_CREAT);
    ck_true(fd >= 0);
    if (fd >= 0) {
        ck(sys_write(fd, (void *)"nested", 6), 6);
        ck(sys_close(fd), 0);
    }
    fd = (int)sys_open(DFILE, O_RDONLY);
    ck_true(fd >= 0);
    if (fd >= 0) {
        for (int i = 0; i < 8; i++) buf[i] = 0;
        ck(sys_read(fd, buf, 6), 6);
        ck_true(streq(buf, "nested"));
        ck(sys_close(fd), 0);
    }
    ck(sys_unlinkat(AT_FDCWD, DFILE, 0), 0);
    ck(sys_rmdir(DSUB), 0);
    ck(sys_rmdir(DDIR), 0);
    ck(sys_open(DSUB, O_RDONLY | O_DIRECTORY), ENOENT);
}

static void dup_test(void) {
    int f = (int)sys_open("/TEST.TXT", O_RDONLY);
    ck_true(f >= 0);
    if (f < 0) return;

    ck(sys_dup2(1, 5, 0), 5);
    ck(sys_isatty(5), 1);
    ck(sys_dup2(f, 5, 0), 5);
    ck(sys_isatty(5), 0);
    ck(sys_fcntl(5, F_GETFD, 0), 0);
    ck(sys_close(5), 0);

    ck(sys_dup2(1, 5, O_CLOEXEC), 5);
    ck(sys_fcntl(5, F_GETFD, 0) & 1, 1);
    ck(sys_close(5), 0);

    int c = (int)sys_open("/TEST.TXT", O_RDONLY | O_CLOEXEC);
    ck_true(c >= 0);
    if (c >= 0) {
        ck(sys_fcntl(c, F_GETFD, 0) & 1, 1);
        int d = (int)sys_dup(c, 0, 0);
        ck_true(d >= 0);
        if (d >= 0) {
            ck(sys_fcntl(d, F_GETFD, 0), 0);
            ck(sys_close(d), 0);
        }
        ck(sys_dup2(c, 6, 0), 6);
        ck(sys_fcntl(6, F_GETFD, 0), 0);
        ck(sys_close(6), 0);
        ck(sys_close(c), 0);
    }

    int hi = (int)sys_fcntl(f, F_DUPFD, 20);
    ck_true(hi >= 20);
    if (hi >= 20) {
        ck(sys_fcntl(hi, F_GETFD, 0), 0);
        ck(sys_close(hi), 0);
    }
    ck(sys_close(f), 0);

    int opened = 0;
    int last = 0;
    for (int i = 0; i < 300; i++) {
        int fd = (int)sys_open("/TEST.TXT", O_RDONLY);
        if (fd < 0) { last = fd; break; }
        opened++;
    }
    ck_true(opened > 3);
    ck(last, EMFILE);
    for (int i = 3; i < 3 + opened; i++) sys_close(i);
}

static void nonblock_test(void) {
    int fd = (int)sys_open("/TEST.TXT", O_RDONLY | O_NONBLOCK);
    ck_true(fd >= 0);
    if (fd < 0) return;
    ck(sys_fcntl(fd, F_GETFL, 0) & O_NONBLOCK, O_NONBLOCK);
    for (int i = 0; i < 9; i++) buf[i] = 0;
    ck(sys_read(fd, buf, 8), 8);
    ck_true(streq(buf, "that my "));
    ck(sys_close(fd), 0);
}

int main(void) {
    for (int i = 0; i < RES_SLOTS; i++) res[i] = RES_PENDING;

    hole_test();
    trunc_test();
    multi_test();
    name_test();
    dir_test();
    dup_test();
    nonblock_test();

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    sys_exit(failed ? 1 : 0);
    return 0;
}
