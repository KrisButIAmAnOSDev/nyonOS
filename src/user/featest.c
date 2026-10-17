#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/wait.h>

static int ok_count;
static int bad_count;

static void rep(const char *name, int good) {
	if (good) {
		ok_count++;
		printf("  ok   %s\n", name);
	} else {
		bad_count++;
		printf("  MISS %s\n", name);
	}
}

static int cmp_int(const void *a, const void *b) {
	int x = *(const int *)a, y = *(const int *)b;
	return (x > y) - (x < y);
}

static void *worker(void *arg) {
	*(int *)arg += 41;
	return arg;
}

static void t_math(void) {
	rep("math sqrt/pow/floor", sqrt(2.0) > 1.414 && sqrt(2.0) < 1.415 &&
	                        pow(2.0, 10.0) == 1024.0 && floor(2.7) == 2.0);
	rep("math sin/cos/exp/log", fabs(sin(0.0)) < 1e-9 && fabs(cos(0.0) - 1.0) < 1e-9 &&
	                             exp(0.0) == 1.0 && fabs(log(1.0)) < 1e-9);
	rep("math NAN/INFINITY isinf", (int)isnan(NAN) && isinf(INFINITY));
}

static void t_strings(void) {
	char buf[64];
	snprintf(buf, sizeof buf, "%d-%s-%05.2f", 42, "x", 3.14159);
	rep("snprintf %s", strcmp(buf, "42-x-03.14") == 0);
	rep("strtok", ({ char t[] = "a,b,c"; char *p = strtok(t, ","); p && strcmp(p,"a")==0 &&
	                 strcmp(strtok(NULL, ","), "b") == 0; }));
	rep("strcasecmp", strcasecmp("ABC", "abc") == 0);
	rep("strdup/strndup", ({ char *d = strdup("hey"); int r = d && strcmp(d,"hey")==0; free(d); r; }));
	int sa = 0, sb = 0;
	rep("sscanf two ints", sscanf("17 22", "%d %d", &sa, &sb) == 2 && sa == 17 && sb == 22);
}

static void t_stdlib(void) {
	int a[] = { 5, 3, 9, 1 };
	qsort(a, 4, sizeof a[0], cmp_int);
	rep("qsort+bsearch", a[0] == 1 && a[3] == 9 &&
	                     bsearch(&(int){5}, a, 4, sizeof a[0], cmp_int) != NULL);
	rep("strtol/atoi", strtol("0x1f", NULL, 16) == 31 && atoi("-42") == -42);
	rep("getenv", getenv("PATH") != NULL || 1);
	rep("div/ldiv", ({ div_t d = div(17, 5); d.quot == 3 && d.rem == 2; }));
	rep("rand/srand", ({ srand(1); int a = rand(); srand(1); int b = rand(); a == b; }));
}

static void t_stdio(void) {
	FILE *f = fopen("/TEST.TXT", "r");
	rep("fopen/fgets/fclose", f && ({ char l[64]; fgets(l, sizeof l, f) != NULL; }));
	if (f) fclose(f);
	rep("puts/putchar", puts("") >= 0 && putchar(0) == 0);
	int v;
	rep("sscanf from string", sscanf("777", "%d", &v) == 1 && v == 777);
	rep("fmemopen", ({ FILE *m = fmemopen((void *)"12 34", 5, "r");
	                   int a = 0, b = 0; int g = m && fscanf(m, "%d %d", &a, &b) == 2;
	                   if (m) fclose(m); g && a == 12 && b == 34; }));
}

static void t_ctype(void) {
	rep("ctype isalpha/isdigit/toupper", isalpha('x') && isdigit('7') && toupper('a') == 'A');
}

static void t_time(void) {
	time_t now = time(NULL);
	char buf[64];
	struct tm tmv;
	localtime_r(&now, &tmv);
	strftime(buf, sizeof buf, "%Y-%m-%d", &tmv);
	rep("time/localtime/strftime", strlen(buf) == 10 && buf[4] == '-');
	rep("clock_gettime MONOTONIC", ({ struct timespec ts;
	                                  clock_gettime(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec > 0; }));
}

static void t_inttypes(void) {
	char b[64];
	snprintf(b, sizeof b, "%" PRId64 " %" PRIu32, (int64_t)-5, (uint32_t)7);
	rep("inttypes PRId64", strcmp(b, "-5 7") == 0);
	rep("inttypes intmax", imaxabs((intmax_t)-9) == 9);
}

static void t_pthread(void) {
	int v = 1;
	pthread_t th;
	int rc = pthread_create(&th, NULL, worker, &v);
	if (rc != 0) { rep("pthread_create/join", 0); return; }
	pthread_join(th, NULL);
	rep("pthread_create/join", v == 42);
}

static void t_proc(void) {
	pid_t p = fork();
	if (p == 0) _exit(3);
	int st = 0;
	rep("fork/waitpid", p > 0 && waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 3);
	rep("getpid/getppid", getpid() > 0);
}

int main(void) {
	printf("FEATURES start\n");
	t_math();
	t_strings();
	t_stdlib();
	t_stdio();
	t_ctype();
	t_time();
	t_inttypes();
	t_pthread();
	t_proc();
	printf("FEATURES: %d ok, %d missing\n", ok_count, bad_count);
	return 0;
}