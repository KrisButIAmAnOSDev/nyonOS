#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <time.h>
#include <sys/time.h>

int main(void) {
	struct timespec mono, real;
	struct timeval tv;

	if (clock_gettime(CLOCK_MONOTONIC, &mono)) {
		printf("CLK: CLOCK_MONOTONIC failed\n");
		return 1;
	}
	printf("monotonic %lld.%09ld\n", (long long)mono.tv_sec, mono.tv_nsec);

	if (clock_gettime(CLOCK_REALTIME, &real)) {
		printf("CLK: CLOCK_REALTIME failed\n");
		return 1;
	}
	printf("realtime  %lld.%09ld\n", (long long)real.tv_sec, real.tv_nsec);

	if (gettimeofday(&tv, NULL)) {
		printf("CLK: gettimeofday failed\n");
		return 1;
	}
	printf("gettimeofday %lld.%06ld\n", (long long)tv.tv_sec, (long long)tv.tv_usec);

	if (real.tv_sec < 1700000000) {
		printf("CLK: realtime looks wrong\n");
		return 1;
	}

	time_t t = time(NULL);
	printf("time()    %lld\n", (long long)t);

	printf("CLK: %s\n", "PASSED");
	return 0;
}
