#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define W 72
#define H 34

int main(void) {
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	long total = 0;
	int max_iter = 0;

	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			double cr = (x - W / 2.0) * 3.0 / (W / 2.0) - 0.5;
			double ci = (y - H / 2.0) * 2.4 / (H / 2.0);
			double zr = 0.0, zi = 0.0;
			int i = 0;
			while (i < 80 && zr * zr + zi * zi <= 4.0) {
				double t = zr * zr - zi * zi + cr;
				zi = 2.0 * zr * zi + ci;
				zr = t;
				i++;
			}
			total += i;
			if (i > max_iter) max_iter = i;
			putchar(i >= 80 ? '#' : " .:-=+*#%@"[i * 9 / 80]);
		}
		putchar('\n');
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

	printf("mandelbrot %dx%d  mean-iter %.2f  max %d  %.1f ms\n",
	       W, H, (double)total / (W * H), max_iter, ms);

	unsigned long long acc = 0;
	for (long long n = 1; n <= 3000000; n++) {
		double s = 0.0, term = 1.0;
		for (int k = 1; k <= 6; k++) { term *= 1.0 / (double)k; s += term; }
		acc += (unsigned long long)(s * 1e9);
	}
	printf("series-sum checksum %llu\n", acc);
	return 0;
}