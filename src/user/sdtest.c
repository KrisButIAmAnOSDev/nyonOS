#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <sched.h>
#include <fcntl.h>
#include <stdio.h>
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

	printf(fails ? "SYSD: FAILED\n" : "SYSD: PASSED\n");
	return 0;
}