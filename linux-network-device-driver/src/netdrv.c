// SPDX-License-Identifier: GPL-2.0
/*
 * netdrv — virtual Ethernet pair (vnet0 <-> vnet1) with NAPI and TX backpressure.
 *
 * Milestones implemented here:
 *   1  Build skeleton: module_init / module_exit, module metadata, Makefile.
 *   2  Core data structures: netdrv_ring, netdrv_stats, netdrv_priv, ring helpers.
 *   3  Register net devices: alloc_netdev, netif_napi_add, tasklet setup,
 *      register_netdev, and the symmetric teardown path.
 *   4  Open / stop paths: NAPI enable/disable, carrier, queue start/stop,
 *      and RX ring drain on stop.
 *   5  TX path: skb_clone into peer RX ring, TX backpressure (stop/wake),
 *      simulated RX interrupt via tasklet -> napi_schedule.
 *   6  Simulated interrupt and NAPI poll: tasklet calls napi_schedule;
 *      netdrv_poll drains the RX ring within budget, delivers skbs via
 *      netif_receive_skb, calls napi_complete_done, and wakes the peer
 *      TX queue when ring space becomes available.
 *   7  Statistics and ethtool: ndo_get_stats64 fills rtnl_link_stats64;
 *      ethtool_ops exposes all driver-private counters via ethtool -S.
 *   8  Backpressure testing: ring_size module parameter (power-of-two,
 *      1–NETDRV_RING_SIZE) lets stress tests use tiny rings to force
 *      frequent stop/wake cycles observable via ethtool -S counters.
 *   9  Teardown / error-path hardening: drain both rings and wake the
 *      peer queue in ndo_stop (not just module exit), so a peer blocked
 *      on this device's RX ring doesn't stay stopped forever after this
 *      device goes down.
 *  10  Scripts: scripts/setup_netns.sh, scripts/teardown_netns.sh, and
 *      scripts/smoke_test.sh automate the manual test steps below.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/log2.h>
#include <linux/skbuff.h>
#include <linux/version.h>
#include "netdrv.h"

#define NETDRV_DRIVER_VERSION "0.3"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Charles Zhao");
MODULE_DESCRIPTION("Virtual Ethernet pair (vnet0 <-> vnet1) with NAPI and TX backpressure");
MODULE_VERSION(NETDRV_DRIVER_VERSION);

/*
 * ring_size — runtime ring capacity for stress testing.
 *
 * Must be a power of two and at most NETDRV_RING_SIZE.  The array in
 * netdrv_ring is always NETDRV_RING_SIZE slots; only the first ring_size
 * slots are used, so the modulo index stays within bounds.
 *
 * Example — force frequent stop/wake cycles:
 *   insmod netdrv.ko ring_size=4
 *   ping -f -c 1000 10.0.0.2
 *   ethtool -S vnet0   # observe tx_queue_stops / tx_queue_wakes
 */
static unsigned int ring_size = NETDRV_RING_SIZE;
module_param(ring_size, uint, 0444);
MODULE_PARM_DESC(ring_size,
	"RX/TX ring capacity — power of two, 1–" __stringify(NETDRV_RING_SIZE)
	" (default " __stringify(NETDRV_RING_SIZE) ")");

static struct net_device *netdrv_devs[2];

/* ---- ring operations -------------------------------------------- */

/*
 * ring_enqueue / ring_dequeue use spin_lock_bh so they are safe whether
 * called from process context (ndo_start_xmit under some schedulers) or
 * softirq context (NAPI poll, tasklet).
 */

static bool ring_enqueue(struct netdrv_ring *ring, struct sk_buff *skb)
{
	bool ok = false;

	spin_lock_bh(&ring->lock);
	if (!ring_full(ring->head, ring->tail, ring_size)) {
		ring->desc[ring->head % ring_size] = skb;
		ring->head++;
		ok = true;
	}
	spin_unlock_bh(&ring->lock);
	return ok;
}

static struct sk_buff *ring_dequeue(struct netdrv_ring *ring)
{
	struct sk_buff *skb = NULL;

	spin_lock_bh(&ring->lock);
	if (!ring_empty(ring->head, ring->tail)) {
		skb = ring->desc[ring->tail % ring_size];
		ring->tail++;
	}
	spin_unlock_bh(&ring->lock);
	return skb;
}

/*
 * ring_drain_free — free every queued skb.  Called only from ndo_stop
 * (after napi_disable) and module_exit (after tasklet_kill + napi_disable),
 * so no concurrent access is possible when this runs.
 */
static void ring_drain_free(struct netdrv_ring *ring)
{
	struct sk_buff *skb;

	while ((skb = ring_dequeue(ring)) != NULL)
		dev_kfree_skb_any(skb);
}

/* ---- simulated RX interrupt (tasklet) --------------------------- */

/*
 * Mirrors a real hardirq handler: do the minimum (schedule NAPI) and return.
 * Kernel 5.9 replaced tasklet_init(unsigned long data) with tasklet_setup,
 * which passes the tasklet pointer itself; from_tasklet() recovers the priv.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
static void netdrv_rx_tasklet(struct tasklet_struct *t)
{
	struct netdrv_priv *priv = from_tasklet(priv, t, rx_tasklet);

	napi_schedule(&priv->napi);
}
#else
static void netdrv_rx_tasklet(unsigned long data)
{
	struct netdrv_priv *priv = (struct netdrv_priv *)data;

	napi_schedule(&priv->napi);
}
#endif

/* ---- NAPI poll -------------------------------------------------- */

static int netdrv_poll(struct napi_struct *napi, int budget)
{
	struct netdrv_priv *priv = container_of(napi, struct netdrv_priv, napi);
	struct netdrv_priv *peer_priv;
	struct sk_buff *skb;
	int work_done = 0;

	spin_lock_bh(&priv->stats_lock);
	priv->stats.napi_polls++;
	spin_unlock_bh(&priv->stats_lock);

	while (work_done < budget) {
		skb = ring_dequeue(&priv->rx_ring);
		if (!skb)
			break;

		skb->protocol = eth_type_trans(skb, priv->dev);

		spin_lock_bh(&priv->stats_lock);
		priv->stats.rx_packets++;
		priv->stats.rx_bytes += skb->len;
		spin_unlock_bh(&priv->stats_lock);

		netif_receive_skb(skb);
		work_done++;
	}

	if (work_done < budget) {
		/*
		 * Ring drained within budget — disable further polling and
		 * wake the peer TX queue if it was stopped waiting on us.
		 */
		napi_complete_done(napi, work_done);

		spin_lock_bh(&priv->stats_lock);
		priv->stats.napi_completions++;
		spin_unlock_bh(&priv->stats_lock);

		if (priv->peer) {
			peer_priv = netdev_priv(priv->peer);
			if (netif_queue_stopped(priv->peer)) {
				netif_wake_queue(priv->peer);
				spin_lock_bh(&peer_priv->stats_lock);
				peer_priv->stats.tx_queue_wakes++;
				spin_unlock_bh(&peer_priv->stats_lock);
			}
		}
	}

	return work_done;
}

/* ---- net_device_ops --------------------------------------------- */

static int netdrv_open(struct net_device *dev)
{
	struct netdrv_priv *priv = netdev_priv(dev);

	napi_enable(&priv->napi);
	netif_start_queue(dev);
	netif_carrier_on(dev);
	return 0;
}

static int netdrv_stop(struct net_device *dev)
{
	struct netdrv_priv *priv = netdev_priv(dev);
	struct netdrv_priv *peer_priv;

	netif_stop_queue(dev);
	netif_carrier_off(dev);
	/*
	 * napi_disable waits for any running poll to finish and prevents new
	 * polls from being scheduled; safe to drain the rings afterwards.
	 */
	napi_disable(&priv->napi);
	ring_drain_free(&priv->rx_ring);
	ring_drain_free(&priv->tx_ring);

	/*
	 * This device's RX ring is now permanently empty until reopened, and
	 * with NAPI disabled the usual wake from netdrv_poll() will never
	 * happen. If the peer's TX queue is stopped waiting for room here,
	 * wake it now so it doesn't stay stopped forever — the peer's next
	 * ndo_start_xmit call will see netif_running(this dev) == false and
	 * drop the packet via the early netif_running check instead.
	 */
	if (priv->peer) {
		peer_priv = netdev_priv(priv->peer);
		if (netif_queue_stopped(priv->peer)) {
			netif_wake_queue(priv->peer);
			spin_lock_bh(&peer_priv->stats_lock);
			peer_priv->stats.tx_queue_wakes++;
			spin_unlock_bh(&peer_priv->stats_lock);
		}
	}

	return 0;
}

static netdev_tx_t netdrv_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct netdrv_priv *priv = netdev_priv(dev);
	struct netdrv_priv *peer_priv;
	struct sk_buff *clone;

	if (!priv->peer || !netif_running(priv->peer)) {
		spin_lock_bh(&priv->stats_lock);
		priv->stats.tx_dropped++;
		spin_unlock_bh(&priv->stats_lock);
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	peer_priv = netdev_priv(priv->peer);

	/*
	 * Pre-check without the lock for a fast early exit when the peer
	 * RX ring is clearly full.  READ_ONCE prevents the compiler from
	 * tearing or re-reading these u32 fields across the comparison.
	 * We still re-check under the ring lock inside ring_enqueue().
	 */
	if (ring_full(READ_ONCE(peer_priv->rx_ring.head),
		      READ_ONCE(peer_priv->rx_ring.tail), ring_size)) {
		netif_stop_queue(dev);
		spin_lock_bh(&priv->stats_lock);
		priv->stats.tx_queue_stops++;
		spin_unlock_bh(&priv->stats_lock);
		spin_lock_bh(&peer_priv->stats_lock);
		peer_priv->stats.rx_ring_full++;
		spin_unlock_bh(&peer_priv->stats_lock);
		/* Return BUSY so the stack retains and retries this skb. */
		return NETDEV_TX_BUSY;
	}

	clone = skb_clone(skb, GFP_ATOMIC);
	if (!clone) {
		spin_lock_bh(&priv->stats_lock);
		priv->stats.tx_dropped++;
		spin_unlock_bh(&priv->stats_lock);
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	/* The clone belongs to the peer's RX path from here on. */
	clone->dev = priv->peer;

	if (!ring_enqueue(&peer_priv->rx_ring, clone)) {
		/*
		 * Ring became full between the pre-check and the locked
		 * enqueue (concurrent TX on peer device).  Stop the queue
		 * and return BUSY so the stack retries the original skb.
		 */
		kfree_skb(clone);
		netif_stop_queue(dev);
		spin_lock_bh(&priv->stats_lock);
		priv->stats.tx_queue_stops++;
		spin_unlock_bh(&priv->stats_lock);
		spin_lock_bh(&peer_priv->stats_lock);
		peer_priv->stats.rx_ring_full++;
		spin_unlock_bh(&peer_priv->stats_lock);
		return NETDEV_TX_BUSY;
	}

	/* Successful handoff — update counters and release the original. */
	spin_lock_bh(&priv->stats_lock);
	priv->stats.tx_packets++;
	priv->stats.tx_bytes += skb->len;
	spin_unlock_bh(&priv->stats_lock);

	dev_kfree_skb(skb);

	/* Simulate the "packet arrived" hardware interrupt. */
	tasklet_schedule(&peer_priv->rx_tasklet);

	/* Proactively stop the queue if the peer RX ring is now full. */
	if (ring_full(READ_ONCE(peer_priv->rx_ring.head),
		      READ_ONCE(peer_priv->rx_ring.tail), ring_size)) {
		netif_stop_queue(dev);
		spin_lock_bh(&priv->stats_lock);
		priv->stats.tx_queue_stops++;
		spin_unlock_bh(&priv->stats_lock);
	}

	return NETDEV_TX_OK;
}

/* ---- stats and ethtool ------------------------------------------ */

/*
 * Snapshot all driver counters under the stats lock so callers always
 * see a consistent set of values.
 */
static void netdrv_read_stats(struct netdrv_priv *priv, struct netdrv_stats *out)
{
	spin_lock_bh(&priv->stats_lock);
	*out = priv->stats;
	spin_unlock_bh(&priv->stats_lock);
}

static void netdrv_get_stats64(struct net_device *dev,
				struct rtnl_link_stats64 *stats)
{
	struct netdrv_priv *priv = netdev_priv(dev);
	struct netdrv_stats s;

	netdrv_read_stats(priv, &s);

	stats->tx_packets = s.tx_packets;
	stats->tx_bytes   = s.tx_bytes;
	stats->rx_packets = s.rx_packets;
	stats->rx_bytes   = s.rx_bytes;
	stats->tx_dropped = s.tx_dropped;
	stats->rx_dropped = s.rx_dropped;
}

/* ---- ethtool ops ------------------------------------------------ */

#define NETDRV_NUM_STATS 11

static const char netdrv_stat_strings[NETDRV_NUM_STATS][ETH_GSTRING_LEN] = {
	"tx_packets",
	"tx_bytes",
	"rx_packets",
	"rx_bytes",
	"tx_dropped",
	"rx_dropped",
	"tx_queue_stops",
	"tx_queue_wakes",
	"napi_polls",
	"napi_completions",
	"rx_ring_full",
};

static void netdrv_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strscpy(info->driver,  "netdrv", sizeof(info->driver));
	strscpy(info->version, NETDRV_DRIVER_VERSION, sizeof(info->version));
}

static void netdrv_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	if (stringset == ETH_SS_STATS)
		memcpy(data, netdrv_stat_strings, sizeof(netdrv_stat_strings));
}

static int netdrv_get_sset_count(struct net_device *dev, int sset)
{
	if (sset == ETH_SS_STATS)
		return NETDRV_NUM_STATS;
	return -EOPNOTSUPP;
}

static void netdrv_get_ethtool_stats(struct net_device *dev,
				     struct ethtool_stats *estats, u64 *data)
{
	struct netdrv_priv *priv = netdev_priv(dev);
	struct netdrv_stats s;

	netdrv_read_stats(priv, &s);

	data[0]  = s.tx_packets;
	data[1]  = s.tx_bytes;
	data[2]  = s.rx_packets;
	data[3]  = s.rx_bytes;
	data[4]  = s.tx_dropped;
	data[5]  = s.rx_dropped;
	data[6]  = s.tx_queue_stops;
	data[7]  = s.tx_queue_wakes;
	data[8]  = s.napi_polls;
	data[9]  = s.napi_completions;
	data[10] = s.rx_ring_full;
}

static const struct ethtool_ops netdrv_ethtool_ops = {
	.get_drvinfo       = netdrv_get_drvinfo,
	.get_strings       = netdrv_get_strings,
	.get_sset_count    = netdrv_get_sset_count,
	.get_ethtool_stats = netdrv_get_ethtool_stats,
};

static const struct net_device_ops netdrv_netdev_ops = {
	.ndo_open	 = netdrv_open,
	.ndo_stop	 = netdrv_stop,
	.ndo_start_xmit	 = netdrv_start_xmit,
	.ndo_get_stats64 = netdrv_get_stats64,
};

/* ---- device setup ----------------------------------------------- */

static void netdrv_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops	 = &netdrv_netdev_ops;
	dev->ethtool_ops = &netdrv_ethtool_ops;
	dev->features	 = 0;
	eth_hw_addr_random(dev);
}

static void netdrv_priv_init(struct netdrv_priv *priv, struct net_device *dev)
{
	priv->dev  = dev;
	priv->peer = NULL;
	spin_lock_init(&priv->rx_ring.lock);
	spin_lock_init(&priv->tx_ring.lock);
	spin_lock_init(&priv->stats_lock);
	memset(&priv->stats, 0, sizeof(priv->stats));
	priv->rx_ring.head = 0;
	priv->rx_ring.tail = 0;
	priv->tx_ring.head = 0;
	priv->tx_ring.tail = 0;
}

/* ---- module init / exit ----------------------------------------- */

static int __init netdrv_init(void)
{
	struct net_device *dev0, *dev1;
	struct netdrv_priv *priv0, *priv1;
	int err;

	if (!ring_size || ring_size > NETDRV_RING_SIZE ||
	    !is_power_of_2(ring_size)) {
		pr_err("netdrv: ring_size=%u is invalid — must be a power of two between 1 and %u\n",
		       ring_size, NETDRV_RING_SIZE);
		return -EINVAL;
	}

	dev0 = alloc_netdev(sizeof(struct netdrv_priv), "vnet%d",
			    NET_NAME_ENUM, netdrv_setup);
	if (!dev0) {
		pr_err("netdrv: alloc_netdev failed for dev0\n");
		return -ENOMEM;
	}

	dev1 = alloc_netdev(sizeof(struct netdrv_priv), "vnet%d",
			    NET_NAME_ENUM, netdrv_setup);
	if (!dev1) {
		pr_err("netdrv: alloc_netdev failed for dev1\n");
		err = -ENOMEM;
		goto err_free_dev0;
	}

	priv0 = netdev_priv(dev0);
	priv1 = netdev_priv(dev1);

	netdrv_priv_init(priv0, dev0);
	netdrv_priv_init(priv1, dev1);

	priv0->peer = dev1;
	priv1->peer = dev0;

	/*
	 * netif_napi_add dropped the weight parameter in kernel 6.1;
	 * use the 4-argument form on older kernels.
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	netif_napi_add(dev0, &priv0->napi, netdrv_poll);
	netif_napi_add(dev1, &priv1->napi, netdrv_poll);
#else
	netif_napi_add(dev0, &priv0->napi, netdrv_poll, NETDRV_NAPI_WEIGHT);
	netif_napi_add(dev1, &priv1->napi, netdrv_poll, NETDRV_NAPI_WEIGHT);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	tasklet_setup(&priv0->rx_tasklet, netdrv_rx_tasklet);
	tasklet_setup(&priv1->rx_tasklet, netdrv_rx_tasklet);
#else
	tasklet_init(&priv0->rx_tasklet, netdrv_rx_tasklet,
		     (unsigned long)priv0);
	tasklet_init(&priv1->rx_tasklet, netdrv_rx_tasklet,
		     (unsigned long)priv1);
#endif

	err = register_netdev(dev0);
	if (err) {
		pr_err("netdrv: register_netdev failed for dev0: %d\n", err);
		goto err_napi_del;
	}

	err = register_netdev(dev1);
	if (err) {
		pr_err("netdrv: register_netdev failed for dev1: %d\n", err);
		goto err_unreg_dev0;
	}

	netdrv_devs[0] = dev0;
	netdrv_devs[1] = dev1;

	pr_info("netdrv: registered %s <-> %s (ring_size=%u)\n",
		dev0->name, dev1->name, ring_size);
	return 0;

	/*
	 * Error unwind. Neither device is open at this point (registration
	 * happens before either is brought up), so napi_enable/ndo_open never
	 * ran — tasklet_kill and netif_napi_del are safe no-ops on a tasklet/
	 * NAPI context that was only ever added, never enabled. Falling
	 * through err_napi_del -> err_free_dev0 unwinds dev1 before dev0,
	 * the reverse of allocation order; unregister_netdev(dev0) above
	 * err_napi_del only runs when dev0 was actually registered.
	 */
err_unreg_dev0:
	unregister_netdev(dev0);
err_napi_del:
	tasklet_kill(&priv1->rx_tasklet);
	tasklet_kill(&priv0->rx_tasklet);
	netif_napi_del(&priv1->napi);
	netif_napi_del(&priv0->napi);
	free_netdev(dev1);
err_free_dev0:
	free_netdev(dev0);
	return err;
}

static void __exit netdrv_exit(void)
{
	int i;

	/*
	 * Tear down in reverse registration order.  unregister_netdev calls
	 * ndo_stop if the device is UP, which disables NAPI and drains the
	 * RX ring.  We then kill the tasklet (waits for any running instance),
	 * delete NAPI, drain any residual skbs, and free the device.
	 */
	for (i = 1; i >= 0; i--) {
		struct net_device *dev = netdrv_devs[i];
		struct netdrv_priv *priv;

		if (!dev)
			continue;

		priv = netdev_priv(dev);
		unregister_netdev(dev);
		tasklet_kill(&priv->rx_tasklet);
		netif_napi_del(&priv->napi);
		ring_drain_free(&priv->rx_ring);
		ring_drain_free(&priv->tx_ring);
		free_netdev(dev);
		netdrv_devs[i] = NULL;
	}

	pr_info("netdrv: unregistered\n");
}

module_init(netdrv_init);
module_exit(netdrv_exit);
