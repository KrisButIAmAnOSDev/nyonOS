#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int counter;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *bump(void *arg) {
	(void)arg;
	for (int i = 0; i < 1000; i++) {
		pthread_mutex_lock(&lock);
		counter++;
		pthread_mutex_unlock(&lock);
	}
	return (void *)(long)7;
}

int main(void) {
	pthread_t a, b;

	if (pthread_create(&a, NULL, bump, NULL)) {
		printf("THREADS: pthread_create a failed\n");
		return 1;
	}
	if (pthread_create(&b, NULL, bump, NULL)) {
		printf("THREADS: pthread_create b failed\n");
		return 1;
	}

	void *ra = NULL, *rb = NULL;
	pthread_join(a, &ra);
	pthread_join(b, &rb);

	printf("counter=%d\n", counter);
	printf("ret a=%ld b=%ld\n", (long)ra, (long)rb);
	printf("THREADS: %s\n", (counter == 2000 && (long)ra == 7 && (long)rb == 7) ? "PASSED" : "FAILED");
	return 0;
}