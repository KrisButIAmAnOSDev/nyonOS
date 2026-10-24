#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void ck(const char *what, long got, long want) {
	if (got == want) {
		printf("PASS %s\n", what);
	} else {
		printf("FAIL %s: got %ld, wanted %ld\n", what, got, want);
		fails++;
	}
}

int main(void) {
	pid_t pid = getpid();
	ck("getpid is positive", pid > 0, 1);
	ck("sched_yield succeeds", sched_yield(), 0);
	ck("getpid is stable", getpid(), pid);

	int d = open("/TEST.TXT", O_RDONLY);
	ck("open /TEST.TXT succeeds", d >= 0, 1);

	int d2 = dup(d);
	ck("dup returns a different fd", d2 >= 0 && d2 != d, 1);

	ck("fcntl F_GETFD succeeds", fcntl(d, F_GETFD) >= 0, 1);
	ck("fcntl F_SETFD round-trips", fcntl(d, F_SETFD, 1) == 0 && (fcntl(d, F_GETFD) & 1) == 1, 1);
	ck("fcntl F_GETFL succeeds", fcntl(d, F_GETFL) >= 0, 1);

	int n = 0;
	struct dirent *e;
	DIR *dir = opendir("/BIN");
	ck("opendir /BIN succeeds", dir != NULL, 1);
	while (dir && (e = readdir(dir)) != NULL)
		n++;
	ck("readdir returned entries", n > 0, 1);
	if (dir)
		closedir(dir);

	ck("mkdir through libc", mkdir("/SDTEST.DIR", 0755), 0);
	ck("mkdir of the same dir fails", mkdir("/SDTEST.DIR", 0755) == -1 && errno == EEXIST, 1);
	ck("rmdir through libc", rmdir("/SDTEST.DIR"), 0);
	ck("rmdir of a missing dir fails", rmdir("/SDTEST.DIR") == -1 && errno == ENOENT, 1);

	int wfd = open("/SDTEST.TXT", O_WRONLY | O_CREAT | O_TRUNC);
	ck("create for writing through libc", wfd >= 0, 1);
	ck("write through libc", write(wfd, "sdtest payload", 14), 14);
	ck("fsync through libc", fsync(wfd), 0);
	close(wfd);

	int rfd = open("/SDTEST.TXT", O_RDONLY);
	ck("reopen for reading through libc", rfd >= 0, 1);
	char rb[32];
	ck("read back through libc", read(rfd, rb, 14), 14);
	rb[14] = 0;
	ck("the payload matches", strcmp(rb, "sdtest payload") == 0, 1);
	close(rfd);

	ck("truncate through libc", truncate("/SDTEST.TXT", 4), 0);
	rfd = open("/SDTEST.TXT", O_RDONLY);
	ck("the truncated size", lseek(rfd, 0, SEEK_END), 4);
	close(rfd);

	ck("unlink through libc", unlink("/SDTEST.TXT"), 0);
	ck("the file is gone", open("/SDTEST.TXT", O_RDONLY) < 0, 1);

	printf(fails ? "SYSD: FAILED\n" : "SYSD: PASSED\n");
	return 0;
}