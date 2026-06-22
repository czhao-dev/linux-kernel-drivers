#ifndef CIRCBUF_H
#define CIRCBUF_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CIRCBUF_DEVICE_NAME "circbuf"

struct circbuf_stats {
	__kernel_size_t capacity;   /* total buffer size in bytes */
	__kernel_size_t used;       /* bytes currently in buffer */
	__kernel_size_t available;  /* bytes of free space */
	unsigned long reads;        /* total read() calls since open */
	unsigned long writes;       /* total write() calls since open */
};

#define CIRCBUF_MAGIC 0xC1

/* Query current buffer occupancy/capacity and operation counters */
#define CIRCBUF_GET_STATS _IOR(CIRCBUF_MAGIC, 1, struct circbuf_stats)

#endif /* CIRCBUF_H */
