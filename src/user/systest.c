#include "res.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

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

    struct stat st;
    ck(fstatat(AT_FDCWD, "/TEST.TXT", &st, 0), 0);
    ck_true(st.st_size == 3000);
    ck_true(S_ISREG(st.st_mode));
    ck(fstatat(AT_FDCWD, "/NOPE.TXT", &st, 0), -1);
    ck(errno == ENOENT, 1);

    char lbuf[64];
    errno = 0;
    ck(readlink("/TEST.TXT", lbuf, sizeof lbuf), -1);
    ck(errno == EINVAL, 1);

    errno = 0;
    ck(symlink("/TEST.TXT", "/S2.TXT"), -1);
    ck(errno == EOPNOTSUPP, 1);

    errno = 0;
    ck(link("/TEST.TXT", "/S3.TXT"), -1);
    ck(errno == EOPNOTSUPP, 1);

    ck(chmod("/TEST.TXT", 0400), 0);
    ck(access("/TEST.TXT", W_OK), -1);
    ck(chmod("/TEST.TXT", 0600), 0);
    ck(access("/TEST.TXT", W_OK), 0);

    int fd = open("/TEST.TXT", O_RDONLY);
    ck_true(fd >= 0);
    if (fd >= 0) {
        ck(fchmod(fd, 0400), 0);
        ck(fchmod(fd, 0600), 0);
        ck(close(fd), 0);
    }

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
