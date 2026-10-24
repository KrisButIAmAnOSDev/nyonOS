#define _DEFAULT_SOURCE 1

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
	if (argc < 3) {
		printf("usage: fdprobe <fd> <open|closed> ...\n");
		return 2;
	}

	int bad = 0;
	for (int i = 1; i + 1 < argc; i += 2) {
		int fd = atoi(argv[i]);
		int want_open = strcmp(argv[i + 1], "open") == 0;
		int r = fcntl(fd, F_GETFD);
		int is_open = r >= 0;
		if (is_open != want_open) {
			printf("fdprobe: fd %d is %s, wanted %s\n", fd,
			       is_open ? "open" : "closed",
			       want_open ? "open" : "closed");
			bad++;
		}
	}

	return bad ? 1 : 0;
}
