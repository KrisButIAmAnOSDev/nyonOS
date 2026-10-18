#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>

static int checks;
static int failures;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) {
		failures++;
		printf("    FAIL %s\n", what);
	}
}

static volatile int child_ran;

int main(void) {
	printf("PROCTEST start\n");

	pid_t me = getpid();
	ck(me > 0, "getpid positive");

	child_ran = 0;
	pid_t pid = fork();
	if (pid == 0) {
		child_ran = 35;
		_exit(5);
	}
	ck(pid > 0, "fork returns child pid in parent");

	int status = 0;
	pid_t got = waitpid(pid, &status, 0);
	ck(got == pid, "waitpid returns the child pid");
	ck(WIFEXITED(status), "child exited normally");
	ck(WEXITSTATUS(status) == 5, "child exit status is 5");
	ck(child_ran == 0, "fork copied the address space, parent kept its own global");

	pid_t any = waitpid(-1, &status, WNOHANG);
	ck(any < 0, "waitpid with no children fails");

	struct timespec req = { 0, 120 * 1000 * 1000 };
	struct timespec t0, after, rem;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (nanosleep(&req, &rem) == 0) {
		clock_gettime(CLOCK_MONOTONIC, &after);
		long long ns = (after.tv_sec - t0.tv_sec) * 1000000000LL
			     + (after.tv_nsec - t0.tv_nsec);
		ck(ns >= 100000000LL, "nanosleep slept at least 100ms");
	} else {
		failures++;
		printf("    FAIL nanosleep returned error\n");
	}

	int fd = open("/TEST.TXT", O_RDONLY);
	if (fd >= 0) {
		int copy = dup(fd);
		ck(copy >= 0, "dup returns a new fd");
		if (copy >= 0) {
			char a[8], b[8];
			ssize_t ra = read(fd, a, 4);
			ssize_t rb = read(copy, b, 4);
			ck(ra == 4 && rb == 4, "dup'd fd reads independently");
			close(fd);
			close(copy);
		} else {
			close(fd);
		}

		int spare = open("/TEST.TXT", O_RDONLY);
		if (spare >= 0) {
			int moved = dup2(spare, 30);
			ck(moved == 30, "dup2 returns the new fd");
			char c1[4];
			ck(read(moved, c1, 4) == 4, "dup2'd fd is readable");
			ck(dup2(moved, moved) == moved, "dup2 onto itself returns the same fd");
			ck(dup2(spare, -1) < 0, "dup2 with a bad target fails");
			close(moved);
			close(spare);
			close(fd);
		} else {
			failures++;
			printf("    FAIL reopen for dup2\n");
			close(fd);
		}
	} else {
		failures++;
		printf("    FAIL open /TEST.TXT\n");
	}

	printf("PROCTEST: %d of %d checks passed\n", checks - failures, checks);
	printf("PROCTEST: %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}