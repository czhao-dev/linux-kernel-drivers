// SPDX-License-Identifier: GPL-2.0
/*
 * pcie_edu_test - userspace driver test program
 *
 * Exercises /dev/pcie_edu one milestone at a time. Run inside the QEMU guest
 * via docker/run.sh test. Each test prints a clear PASS/FAIL line so
 * docker/init.sh can confirm the README's Testing Strategy items.
 *
 * Milestones covered:
 *   M1/M2: open device, liveness register round-trip
 *   M4:    PCIE_EDU_COMPUTE ioctl — blocking factorial via MSI interrupt
 *   M5:    PCIE_EDU_DMA_TRANSFER ioctl — DMA round-trip correctness check
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../pcie_edu.h"

#define DEVICE "/dev/pcie_edu"

static int fd;

static void die(const char *msg)
{
	perror(msg);
	_exit(1);
}

/* M1: confirm the device node exists and can be opened */
static void test_open(void)
{
	fd = open(DEVICE, O_RDWR);
	if (fd < 0)
		die("open " DEVICE);
	printf("PASS: open %s\n", DEVICE);
}

/* M2: write X to the liveness register, read back ~X */
static void test_liveness(void)
{
	uint32_t write_val = 0x12345678;
	uint32_t read_val;

	if (write(fd, &write_val, sizeof(write_val)) != (ssize_t)sizeof(write_val))
		die("write liveness");

	if (read(fd, &read_val, sizeof(read_val)) != (ssize_t)sizeof(read_val))
		die("read liveness");

	if (read_val != ~write_val) {
		fprintf(stderr,
			"FAIL: liveness: wrote 0x%08x, read 0x%08x, expected 0x%08x\n",
			write_val, read_val, ~write_val);
		_exit(1);
	}

	printf("PASS: liveness register: wrote 0x%08x, read back 0x%08x (~write_val)\n",
	       write_val, read_val);
}

/* M4: verify factorial results via the blocking PCIE_EDU_COMPUTE ioctl */
static void test_compute(void)
{
	static const struct { uint32_t n; uint32_t expected; } cases[] = {
		{ 0,  1 },
		{ 1,  1 },
		{ 5,  120 },
		{ 10, 3628800 },
		{ 12, 479001600 },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t val = cases[i].n;

		if (ioctl(fd, PCIE_EDU_COMPUTE, &val) != 0)
			die("ioctl PCIE_EDU_COMPUTE");

		if (val != cases[i].expected) {
			fprintf(stderr,
				"FAIL: compute: %u! = %u, expected %u\n",
				cases[i].n, val, cases[i].expected);
			_exit(1);
		}

		printf("PASS: %u! = %u\n", cases[i].n, val);
	}
}

/* M5: DMA round-trip — kernel fills buffer, sends to device, reads back, verifies */
static void test_dma(void)
{
	struct pcie_edu_dma_xfer xfer = { .len = 4096, .pattern = 0xAB };

	if (ioctl(fd, PCIE_EDU_DMA_TRANSFER, &xfer) != 0) {
		perror("ioctl PCIE_EDU_DMA_TRANSFER");
		fprintf(stderr, "FAIL: DMA round-trip (pattern=0x%02x len=%u)\n",
			xfer.pattern, xfer.len);
		_exit(1);
	}

	printf("PASS: DMA round-trip (pattern=0x%02x len=%u)\n",
	       xfer.pattern, xfer.len);
}

int main(void)
{
	test_open();
	test_liveness();
	test_compute();
	test_dma();

	puts("ALL TESTS PASSED");
	return 0;
}
