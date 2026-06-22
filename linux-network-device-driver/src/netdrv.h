/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NETDRV_H
#define _NETDRV_H

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>

#define NETDRV_RING_SIZE    256
#define NETDRV_NAPI_WEIGHT  64

/*
 * Fixed-size circular descriptor ring.  head points to the next slot to
 * write; tail points to the next slot to read.  The ring is full when
 * (head - tail) == NETDRV_RING_SIZE and empty when head == tail.
 * u32 wrap-around makes the subtraction correct without extra masking.
 */
struct netdrv_ring {
	struct sk_buff	*desc[NETDRV_RING_SIZE];
	u32		 head;
	u32		 tail;
	spinlock_t	 lock;
};

struct netdrv_stats {
	u64 tx_packets;
	u64 tx_bytes;
	u64 rx_packets;
	u64 rx_bytes;
	u64 tx_dropped;
	u64 rx_dropped;
	u64 tx_queue_stops;
	u64 tx_queue_wakes;
	u64 napi_polls;
	u64 napi_completions;
	/* incremented on the RX ring owner when a TX enqueue is rejected */
	u64 rx_ring_full;
};

struct netdrv_priv {
	struct net_device	*dev;
	struct net_device	*peer;
	struct napi_struct	 napi;
	struct tasklet_struct	 rx_tasklet;
	struct netdrv_ring	 rx_ring;
	struct netdrv_ring	 tx_ring;
	struct netdrv_stats	 stats;
	spinlock_t		 stats_lock;
};

/*
 * ring_full / ring_empty take an explicit size so callers can pass
 * the runtime ring_size module parameter rather than the compile-time
 * NETDRV_RING_SIZE maximum.
 */
static inline bool ring_full(u32 head, u32 tail, u32 size)
{
	return (head - tail) >= size;
}

static inline bool ring_empty(u32 head, u32 tail)
{
	return head == tail;
}

#endif /* _NETDRV_H */
