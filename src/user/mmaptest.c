#include "res.h"
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>

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

#define MFILE "/MAPFILE.BIN"
#define MSIZE 8192

static char pattern[MSIZE];

static void fill_pattern(void) {
    for (int i = 0; i < MSIZE; i++)
        pattern[i] = (char)('A' + (i % 26));
}

static int write_file(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, (const char *)data + off, len - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static long read_file(const char *path, void *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, (char *)buf + off, len - off);
        if (r <= 0) break;
        off += (size_t)r;
    }
    close(fd);
    return (long)off;
}


static long memfree_kb(void) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    char b[4096];
    size_t off = 0;
    ssize_t r;
    while (off < sizeof(b) - 1 && (r = read(fd, b + off, sizeof(b) - 1 - off)) > 0)
        off += (size_t)r;
    close(fd);
    b[off] = 0;

    const char *p = strstr(b, "MemFree:");
    if (!p) return -1;
    p += 8;
    while (*p == ' ' || *p == '\t') p++;
    long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v;
}

int main(void) {
    res_reset_pending(res);
    fill_pattern();

    ck(write_file(MFILE, pattern, MSIZE), 0);

    {
        int fd = open(MFILE, O_RDONLY);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(NULL, MSIZE, PROT_READ, MAP_SHARED, fd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                ck_true(memcmp(m, pattern, MSIZE) == 0);
                ck_true(((char *)m)[0] == 'A');
                ck_true(((char *)m)[MSIZE - 1] == 'A' + ((MSIZE - 1) % 26));
                ck(munmap(m, MSIZE), 0);
            }
            close(fd);
        }
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(NULL, MSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                memcpy(m, "MAPPED", 6);
                ck_true(memcmp(m, "MAPPED", 6) == 0);
                ck(munmap(m, MSIZE), 0);
            }
            close(fd);
        }
    }

    {
        char buf[MSIZE];
        long n = read_file(MFILE, buf, MSIZE);
        ck(n, MSIZE);
        ck_true(memcmp(buf, "MAPPED", 6) == 0);
        ck_true(memcmp(buf + 6, pattern + 6, MSIZE - 6) == 0);
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(NULL, MSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                memcpy(m, "PRIVATE", 7);
                ck_true(memcmp(m, "PRIVATE", 7) == 0);
                ck(munmap(m, MSIZE), 0);
            }
            close(fd);
        }
    }

    {
        char buf[MSIZE];
        long n = read_file(MFILE, buf, MSIZE);
        ck(n, MSIZE);
        ck_true(memcmp(buf, "MAPPED", 6) == 0);
        ck_true(memcmp(buf, "PRIVATE", 7) != 0);
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(NULL, MSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                memset(m, 'Z', 16);
                ck_true(((char *)m)[0] == 'Z');
                ck(munmap(m, MSIZE), 0);
            }
            close(fd);
        }
    }

    {
        pid_t p = fork();
        ck_true(p >= 0);
        if (p == 0) {
            int cfd = open(MFILE, O_RDWR);
            if (cfd >= 0) {
                void *cm = mmap(NULL, MSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
                if (cm != MAP_FAILED) {
                    memcpy(cm, "CHILD", 5);
                    munmap(cm, MSIZE);
                }
                close(cfd);
            }
            _exit(0);
        }
        if (p > 0) {
            int st = 0;
            waitpid(p, &st, 0);
            ck_true(WIFEXITED(st));
            ck(WEXITSTATUS(st), 0);

            int fd = open(MFILE, O_RDONLY);
            ck_true(fd >= 0);
            if (fd >= 0) {
                void *m = mmap(NULL, MSIZE, PROT_READ, MAP_SHARED, fd, 0);
                ck_true(m != MAP_FAILED);
                if (m != MAP_FAILED) {
                    ck_true(memcmp(m, "CHILD", 5) == 0);
                    ck(munmap(m, MSIZE), 0);
                }
                close(fd);
            }
        }
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(NULL, MSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                ck_true(memcmp(m, "CHILD", 5) == 0);
                ck_true(memcmp(m + 5, pattern + 5, MSIZE - 5) != 0);
                munmap(m, MSIZE);
            }
            close(fd);
        }
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            ck_true(write(fd, "PLAIN", 5) == 5);
            close(fd);
        }
        int rfd = open(MFILE, O_RDONLY);
        ck_true(rfd >= 0);
        if (rfd >= 0) {
            void *m = mmap(NULL, MSIZE, PROT_READ, MAP_SHARED, rfd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                ck_true(memcmp(m, "PLAIN", 5) == 0);
                munmap(m, MSIZE);
            }
            close(rfd);
        }
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(NULL, MSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                memcpy(m, "SYNCED", 6);
                ck(sys_msync((uint64_t)(uintptr_t)m, MSIZE, 4), 0);
                char sbuf[MSIZE];
                long sn = read_file(MFILE, sbuf, MSIZE);
                ck(sn, MSIZE);
                ck_true(memcmp(sbuf, "SYNCED", 6) == 0);
                ck(munmap(m, MSIZE), 0);
            }
            close(fd);
        }
    }

    {
        void *want = (void *)0x200000000UL;
        int fd = open(MFILE, O_RDONLY);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(want, 4096, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
            ck_true(m != MAP_FAILED);
            ck_true(m == want);
            if (m != MAP_FAILED) {
                ck_true(((char *)m)[0] == 'S');
                ck(munmap(m, 4096), 0);
            }
            close(fd);
        }
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            char before[MSIZE];
            long bn = read_file(MFILE, before, MSIZE);
            ck(bn, MSIZE);

            void *pm = mmap(NULL, MSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
            ck_true(pm != MAP_FAILED);
            if (pm != MAP_FAILED) {
                memcpy(pm, "PRIVATE!", 8);
                ck_true(memcmp(pm, "PRIVATE!", 8) == 0);
                ck(munmap(pm, MSIZE), 0);
            }

            char after[MSIZE];
            long an = read_file(MFILE, after, MSIZE);
            ck(an, MSIZE);
            ck_true(memcmp(after, before, MSIZE) == 0);

            int sfd = open(MFILE, O_RDONLY);
            ck_true(sfd >= 0);
            if (sfd >= 0) {
                void *sm = mmap(NULL, MSIZE, PROT_READ, MAP_SHARED, sfd, 0);
                ck_true(sm != MAP_FAILED);
                if (sm != MAP_FAILED) {
                    ck_true(memcmp(sm, before, MSIZE) == 0);
                    ck(munmap(sm, MSIZE), 0);
                }
                close(sfd);
            }
            close(fd);
        }
    }

    {
        int fd = open(MFILE, O_RDWR);
        ck_true(fd >= 0);
        if (fd >= 0) {
            void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ck_true(m != MAP_FAILED);
            if (m != MAP_FAILED) {
                int64_t m2 = sys_mremap((uint64_t)(uintptr_t)m, 4096, 8192, 1);
                ck_true(m2 > 0);
                if (m2 > 0) {
                    ck_true(((char *)(uintptr_t)m2)[0] == 'S');
                    ck(sys_msync((uint64_t)(uintptr_t)m2, 8192, 4), 0);
                    ck(munmap((void *)(uintptr_t)m2, 8192), 0);
                }
            }
            close(fd);
        }
    }


    {
        const char *cpath = "/BIN/BASH";
        int sfd = open(cpath, O_RDONLY);
        ck_true(sfd >= 0);
        if (sfd >= 0) {
            void *sm = mmap(NULL, 1024 * 1024, PROT_READ, MAP_SHARED, sfd, 0);
            ck_true(sm != MAP_FAILED);
            if (sm != MAP_FAILED) {
                ck_true(((char *)sm)[0] != 0);
                ck(munmap(sm, 1024 * 1024), 0);
            }
            close(sfd);
        }

        long warm = memfree_kb();
        ck_true(warm > 0);

        int pfd = open(cpath, O_RDONLY);
        ck_true(pfd >= 0);
        if (pfd >= 0) {
            void *pm = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE, pfd, 0);
            ck_true(pm != MAP_FAILED);
            if (pm != MAP_FAILED) {
                long after = memfree_kb();
                ck_true(after > 0);
                long drop = warm - after;
                ck_true(drop < 512);
                ck_true(drop > -512);
                ((char *)pm)[0] = 'P';
                ck_true(((char *)pm)[0] == 'P');
                ck(munmap(pm, 1024 * 1024), 0);
            }
            close(pfd);
        }
    }

    ck(unlink(MFILE), 0);

    res[RES_DONE] = failed ? RES_FAILED : RES_ALLPASS;
    return failed ? 1 : 0;
}
