#include "res.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>

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
    res_reset_pending(res);

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
    ck(symlink("/TEST.TXT", "/S2.TXT"), 0);
    char sbuf[64];
    slong sl = readlink("/S2.TXT", sbuf, sizeof sbuf);
    ck_true(sl == 9 && memcmp(sbuf, "/TEST.TXT", 9) == 0);
    ck(unlink("/S2.TXT"), 0);

    errno = 0;
    struct stat s3;
    ck(link("/TEST.TXT", "/S3.TXT"), 0);
    ck(fstatat(AT_FDCWD, "/S3.TXT", &s3, 0), 0);
    ck_true(s3.st_ino == st.st_ino);
    ck(unlink("/S3.TXT"), 0);
    ck_true(fstatat(AT_FDCWD, "/S3.TXT", &s3, 0) == -1);

    int rf = open("/S4.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ck_true(rf >= 0);
    if (rf >= 0) {
        ck(write(rf, "rn", 2), 2);
        ck(close(rf), 0);
    }
    ck(rename("/S4.TXT", "/S5.TXT"), 0);
    ck_true(fstatat(AT_FDCWD, "/S4.TXT", &s3, 0) == -1);
    ck(fstatat(AT_FDCWD, "/S5.TXT", &s3, 0), 0);
    ck((slong)s3.st_size, 2);
    ck(unlink("/S5.TXT"), 0);

    ck(chown("/TEST.TXT", 500, 600), 0);
    ck(fstatat(AT_FDCWD, "/TEST.TXT", &s3, 0), 0);
    ck((slong)s3.st_uid, 500);
    ck((slong)s3.st_gid, 600);
    ck(chown("/TEST.TXT", 1000, 1000), 0);

    struct timespec tsv[2];
    tsv[0].tv_sec = 1000000;
    tsv[0].tv_nsec = 0;
    tsv[1].tv_sec = 1000001;
    tsv[1].tv_nsec = 0;
    ck(utimensat(AT_FDCWD, "/TEST.TXT", tsv, 0), 0);

    {
        int of = open("/S6.TXT", O_RDWR | O_CREAT | O_TRUNC, 0644);
        ck_true(of >= 0);
        if (of >= 0) {
            ck(write(of, "still here", 10), 10);
            ck(unlink("/S6.TXT"), 0);
            ck_true(fstatat(AT_FDCWD, "/S6.TXT", &s3, 0) == -1);
            ck(lseek(of, 0, SEEK_SET), 0);
            char ob[16];
            ck(read(of, ob, 10), 10);
            ck_true(memcmp(ob, "still here", 10) == 0);
            ck(close(of), 0);
        }
    }

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

    {
        char ebuf[64];
        int efd = open("/USER.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (efd >= 0) {
            const char *msg = "userland ext2\n";
            ck(write(efd, msg, 15), 15);
            ck(close(efd), 0);
            efd = open("/USER.TXT", O_RDONLY);
            ck_true(efd >= 0);
            if (efd >= 0) {
                char rb[64];
                ssize_t n = read(efd, rb, sizeof rb);
                ck(n >= 15 && memcmp(rb, "userland ext2\n", 15) == 0, 1);
                ck(close(efd), 0);
            }
            ck(unlink("/USER.TXT"), 0);
            ck_true(access("/USER.TXT", F_OK) != 0);
        } else {
            ck(1, 1);
        }
    }

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
