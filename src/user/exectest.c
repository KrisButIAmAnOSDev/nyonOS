#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int checks;
static int failures;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) {
		failures++;
		printf("    FAIL %s\n", what);
	}
}

int main(int argc, char **argv) {
	printf("EXEC start\n");

	char *args[3];
	args[0] = (char *)"/BIN/HELLO";
	args[1] = (char *)"--execve";
	args[2] = NULL;

	char *env[2];
	env[0] = (char *)"EXEC_TEST=1";
	env[1] = NULL;

	pid_t p = fork();
	if (p == 0) {
		execve(args[0], args, env);
		printf("    execve returned errno=%d\n", errno);
		_exit(97);
	}

	ck(p > 0, "fork before execve");
	int st = 0;
	pid_t g = waitpid(p, &st, 0);
	ck(g == p, "waitpid returns the exec'd child");
	ck(WIFEXITED(st), "exec'd child exited normally");
	printf("EXEC: child status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	ck(WEXITSTATUS(st) != 97, "execve did not return early");

	pid_t q = fork();
	if (q == 0) {
		execve("/BIN/NOSUCH", args, env);
		_exit(98);
	}
	int st2 = 0;
	waitpid(q, &st2, 0);
	ck(WEXITSTATUS(st2) == 98, "execve of a missing file returns to the caller");

	fflush(stdout);
	int rc = system("/BIN/HELLO");
	printf("EXEC: system(\"/BIN/HELLO\")=%d\n", rc);
	ck(rc == (127 << 8), "system() reaches the shell, which is not installed yet");

	fflush(stdout);
	int pr = system("/BIN/NOSUCHPROG");
	printf("EXEC: system(bad)=%d\n", pr);
	ck(pr != 0, "system() reports failure when no shell can run the command");

	printf("EXEC: %d of %d checks passed\n", checks - failures, checks);
	printf("EXEC: %s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
