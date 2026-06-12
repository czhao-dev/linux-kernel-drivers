/* basic_test - open/write/read/ioctl smoke test for circbuf */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "circbuf.h"

#define MSG "kernel boundary test"

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "/dev/circbuf";

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	ssize_t written = write(fd, MSG, strlen(MSG));
	if (written != (ssize_t)strlen(MSG)) {
		fprintf(stderr, "write: expected %zu bytes, got %zd\n",
			strlen(MSG), written);
		close(fd);
		return 1;
	}

	char buf[64] = {0};
	ssize_t got = read(fd, buf, sizeof(buf) - 1);
	if (got != written || memcmp(buf, MSG, (size_t)got) != 0) {
		fprintf(stderr, "read mismatch: got %zd bytes: \"%s\"\n", got, buf);
		close(fd);
		return 1;
	}
	printf("read back: \"%s\" (%zd bytes)\n", buf, got);

	struct circbuf_stats stats;
	if (ioctl(fd, CIRCBUF_GET_STATS, &stats) < 0) {
		perror("ioctl(CIRCBUF_GET_STATS)");
		close(fd);
		return 1;
	}
	printf("capacity=%zu used=%zu available=%zu reads=%lu writes=%lu\n",
	       (size_t)stats.capacity, (size_t)stats.used, (size_t)stats.available,
	       stats.reads, stats.writes);

	if (stats.used != 0 || stats.reads < 1 || stats.writes < 1) {
		fprintf(stderr, "unexpected stats after balanced read/write\n");
		close(fd);
		return 1;
	}

	close(fd);
	printf("basic_test: PASS\n");
	return 0;
}
