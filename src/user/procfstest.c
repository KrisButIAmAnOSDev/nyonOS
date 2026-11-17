#include "res.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
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

static void ck_true(int cond) {
    if (cond) ok();
    else bad(0, 1);
}

static int has(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++) {
        if (!strncmp(p, needle, nl)) return 1;
    }
    return 0;
}

static char buf[8192];

static long slurp(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    for (;;) {
        if (off >= sizeof(buf) - 1) break;
        ssize_t r = read(fd, buf + off, sizeof(buf) - 1 - off);
        if (r <= 0) break;
        off += (size_t)r;
    }
    close(fd);
    buf[off] = 0;
    return (long)off;
}

static long slurp_at(const char *path, char *dst, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    for (;;) {
        if (off >= cap - 1) break;
        ssize_t r = read(fd, dst + off, cap - 1 - off);
        if (r <= 0) break;
        off += (size_t)r;
    }
    close(fd);
    dst[off] = 0;
    return (long)off;
}

static int dirlist_has(const char *path, const char *want) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;

    struct sys_dirent d;
    int found = 0;
    for (unsigned i = 0; i < 512; i++) {
        slong r = sys_getdents(fd, i, &d);
        if (r <= 0) break;
        if (!strcmp((const char *)d.name, want)) { found = 1; break; }
    }
    close(fd);
    return found;
}

int main(void) {
    res_reset_pending(res);

    long n = slurp("/proc/meminfo");
    ck_true(n > 0);
    ck_true(has(buf, "MemTotal:"));
    ck_true(has(buf, "MemFree:"));
    ck_true(has(buf, "kB"));

    n = slurp("/proc/uptime");
    ck_true(n > 0);
    ck_true(has(buf, "."));

    n = slurp("/proc/version");
    ck_true(n > 0);
    ck_true(has(buf, "nyonOS"));

    n = slurp("/proc/cpuinfo");
    ck_true(n > 0);
    ck_true(has(buf, "processor"));
    ck_true(has(buf, "flags"));

    n = slurp("/proc/stat");
    ck_true(n > 0);
    ck_true(has(buf, "cpu"));
    ck_true(has(buf, "ctxt"));
    ck_true(has(buf, "processes"));

    n = slurp("/proc/loadavg");
    ck_true(n > 0);
    ck_true(has(buf, "/"));

    n = slurp("/proc/self/stat");
    ck_true(n > 0);
    ck_true(has(buf, "("));
    ck_true(has(buf, ")"));

    char pidbuf[16];
    snprintf(pidbuf, sizeof pidbuf, "%d", (int)getpid());
    ck_true(has(buf, pidbuf));

    n = slurp("/proc/self/status");
    ck_true(n > 0);
    ck_true(has(buf, "Name:"));
    ck_true(has(buf, "State:"));
    ck_true(has(buf, "Pid:"));
    ck_true(has(buf, pidbuf));

    n = slurp("/proc/self/cmdline");
    ck_true(n > 0);

    char path[64];
    snprintf(path, sizeof path, "/proc/%s/stat", pidbuf);
    n = slurp(path);
    ck_true(n > 0);
    ck_true(has(buf, pidbuf));

    snprintf(path, sizeof path, "/proc/%s/cmdline", pidbuf);
    n = slurp(path);
    ck_true(n > 0);

    ck_true(dirlist_has("/proc", "self") == 1);
    ck_true(dirlist_has("/proc", "meminfo") == 1);
    ck_true(dirlist_has("/proc", "uptime") == 1);
    ck_true(dirlist_has("/proc", "version") == 1);
    ck_true(dirlist_has("/proc", "cpuinfo") == 1);
    ck_true(dirlist_has("/proc", "stat") == 1);
    ck_true(dirlist_has("/proc", "loadavg") == 1);
    ck_true(dirlist_has("/proc", "nosuchfile") == 0);

    ck_true(dirlist_has("/proc/self", "stat") == 1);
    ck_true(dirlist_has("/proc/self", "status") == 1);
    ck_true(dirlist_has("/proc/self", "cmdline") == 1);

    int fd = open("/proc/nosuchfile", O_RDONLY);
    ck_true(fd < 0);
    if (fd >= 0) close(fd);

    fd = open("/proc/self/nosuchfile", O_RDONLY);
    ck_true(fd < 0);
    if (fd >= 0) close(fd);

    {
        char link[256];
        ssize_t ln = readlink("/proc/self/exe", link, sizeof(link) - 1);
        ck_true(ln > 0);
        if (ln > 0) {
            link[ln] = 0;
            ck_true(link[0] == '/');
            ck_true(strstr(link, "proc") == NULL);
        }
        ln = readlink("/proc/self/nosuch", link, sizeof(link) - 1);
        ck_true(ln < 0);
    }

    fd = open("/proc", O_RDONLY | O_DIRECTORY);
    ck_true(fd >= 0);
    if (fd >= 0) close(fd);

    fd = open("/proc", O_RDONLY);
    ck_true(fd < 0);
    if (fd >= 0) close(fd);

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
