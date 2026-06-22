/*
 * stress - concurrent writer/reader stress test for circbuf
 *
 * Spawns N writer threads and M reader threads operating on the device
 * simultaneously for a fixed duration, then verifies that total bytes
 * read equals total bytes written (no bytes lost or duplicated).
 *
 * Usage: stress [device] --writers=N --readers=M --duration=S
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WRITE_CHUNK 64
#define READ_CHUNK 256
#define DRAIN_ATTEMPTS 100

static const char *device_path = "/dev/circbuf";
static int duration_secs = 10;
static int num_writers = 4;
static int num_readers = 4;

static atomic_ullong total_written;
static atomic_ullong total_read;
static atomic_int writers_active;

static double now_secs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *writer_thread(void *arg)
{
	(void)arg;
	int fd = open(device_path, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("writer: open");
		atomic_fetch_sub(&writers_active, 1);
		return NULL;
	}

	char chunk[WRITE_CHUNK];
	memset(chunk, 'A', sizeof(chunk));

	double end = now_secs() + duration_secs;
	while (now_secs() < end) {
		ssize_t n = write(fd, chunk, sizeof(chunk));

		if (n > 0)
			atomic_fetch_add(&total_written, (unsigned long long)n);
		else if (n < 0 && errno == EAGAIN)
			usleep(500);
		else if (n < 0) {
			perror("writer: write");
			break;
		}
	}

	close(fd);
	atomic_fetch_sub(&writers_active, 1);
	return NULL;
}

static void *reader_thread(void *arg)
{
	(void)arg;
	int fd = open(device_path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("reader: open");
		return NULL;
	}

	char buf[READ_CHUNK];
	int empty_polls = 0;

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n > 0) {
			atomic_fetch_add(&total_read, (unsigned long long)n);
			empty_polls = 0;
		} else if (n < 0 && errno == EAGAIN) {
			if (atomic_load(&writers_active) == 0 &&
			    ++empty_polls >= DRAIN_ATTEMPTS)
				break;
			usleep(500);
		} else if (n < 0) {
			perror("reader: read");
			break;
		} else {
			break; /* EOF, not expected for this device */
		}
	}

	close(fd);
	return NULL;
}

static int parse_args(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--writers=", 10) == 0)
			num_writers = atoi(argv[i] + 10);
		else if (strncmp(argv[i], "--readers=", 10) == 0)
			num_readers = atoi(argv[i] + 10);
		else if (strncmp(argv[i], "--duration=", 11) == 0)
			duration_secs = atoi(argv[i] + 11);
		else if (argv[i][0] != '-')
			device_path = argv[i];
		else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (parse_args(argc, argv) < 0)
		return 1;

	if (num_writers <= 0 || num_readers <= 0 || duration_secs <= 0) {
		fprintf(stderr, "writers/readers/duration must all be positive\n");
		return 1;
	}

	printf("device=%s writers=%d readers=%d duration=%ds\n",
	       device_path, num_writers, num_readers, duration_secs);

	atomic_store(&writers_active, num_writers);

	pthread_t *writers = calloc((size_t)num_writers, sizeof(pthread_t));
	pthread_t *readers = calloc((size_t)num_readers, sizeof(pthread_t));

	for (int i = 0; i < num_readers; i++)
		pthread_create(&readers[i], NULL, reader_thread, NULL);
	for (int i = 0; i < num_writers; i++)
		pthread_create(&writers[i], NULL, writer_thread, NULL);

	for (int i = 0; i < num_writers; i++)
		pthread_join(writers[i], NULL);
	for (int i = 0; i < num_readers; i++)
		pthread_join(readers[i], NULL);

	free(writers);
	free(readers);

	unsigned long long w = atomic_load(&total_written);
	unsigned long long r = atomic_load(&total_read);

	printf("total_written=%llu total_read=%llu\n", w, r);

	if (w != r) {
		fprintf(stderr, "stress: FAIL (diff=%lld)\n", (long long)(w - r));
		return 1;
	}

	printf("stress: PASS\n");
	return 0;
}
