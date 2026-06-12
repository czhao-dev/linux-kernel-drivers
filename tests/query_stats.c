/* query_stats - print circbuf buffer statistics via ioctl */

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "circbuf.h"

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "/dev/circbuf";

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	struct circbuf_stats stats;
	if (ioctl(fd, CIRCBUF_GET_STATS, &stats) < 0) {
		perror("ioctl(CIRCBUF_GET_STATS)");
		close(fd);
		return 1;
	}

	printf("capacity:  %zu\n", (size_t)stats.capacity);
	printf("used:      %zu\n", (size_t)stats.used);
	printf("available: %zu\n", (size_t)stats.available);
	printf("reads:     %lu\n", stats.reads);
	printf("writes:    %lu\n", stats.writes);

	close(fd);
	return 0;
}
