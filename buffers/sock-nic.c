/* Simone Nellaga, GNU GPLv2 or later */

/*
 *This a buffer implementation for the new PF_ZIO protocol family, allowing
 * users to exchange data with the framework using sockets. So it implements a
 * new software network interface, and a new socket-layer logic.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/tcp_states.h> /* TCP_ESABLISHED is needed for SOCK_STREAM */
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/net.h>
#include <linux/device.h>

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>

#define PF_ZIO			28
#define AF_ZIO			28
#define SOL_ZIO			281

struct net_device *netdev;

static int zn_open(struct net_device *dev)
{
	printk("%s called\n", __func__);
	netif_start_queue(dev);
	return 0;
}

static int zn_close(struct net_device *dev)
{
	printk("%s called\n", __func__);
	netif_stop_queue(dev);
	return 0;
}

static int zn_hard_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct zn_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	struct zio_sock *zsk;
	struct zio_dest *d;
	struct zio_block *block;
	struct zio_cb *cb = (struct zio_cb *)skb->cb;

	printk("%s called\n", __func__);

	if (unlikely(skb->protocol != cpu_to_be16(ETH_P_ZIO))) {
		/*FIXME kfree_skb needed?*/
		return NET_XMIT_DROP;
	}

	/*Ugly solution to the problem exposed in zn_sendmsg*/
	zsk = cb->zsk;
	block = cb->block;

	if (cb->flags & ZSOCK_SENDTO)
		d = &zsk->sendto_chan;
	else
		d = &zsk->connected_chan;

	if ((d->ti->flags & ZIO_STATUS) == ZIO_DISABLED) {
		printk("Zio trigger is disabled, ");
		goto out_free;
	}
	if (d->ti->t_op->push_block(d->ti, d->chan, block) < 0)
		if (d->bi->b_op->store_block(d->bi, block) < 0){
			printk("Not enough space in buffer, ");
			goto out_free;
		}

	/*Update stats*/
	stats->tx_packets++;
	stats->tx_bytes += skb->len;
	dev->trans_start = jiffies;
	return NETDEV_TX_OK;

/*FIXME Like this?*/
out_free:
	stats->tx_dropped++;
	d->bi->b_op->free_block(d->bi, block);
	return NET_XMIT_DROP;
}

static int zn_set_mac_address(struct net_device *dev, void *vaddr)
{
	printk("%s called\n", __func__);
	return 0;
}

/*zn_header stands for the device->hard_header. This function is called before
 * transmission and should build the header for the packet*/

static int zn_header(struct sk_buff *skb, struct net_device *dev,
				unsigned short type, const void *daddr,
					const void *saddr, unsigned int len)
{
	struct ethhdr *eth;

	if (type != ETH_P_ZIO)
		return 0;

	eth = (struct ethhdr *)skb_push(skb, sizeof(struct ethhdr));
	eth->h_proto = htons(type);

	memcpy(eth->h_source, saddr, ETH_ALEN);
	memcpy(eth->h_dest, daddr, ETH_ALEN);

	return 0;
}

static int zn_rebuild_header(struct sk_buff *skb)
{
	printk("%s called\n", __func__);
	return 0;
}

static void zn_tx_timeout(struct net_device *dev)
{
	printk("%s called\n", __func__);
	return;
}

struct net_device_stats *zn_stats(struct net_device *dev)
{
	struct zn_priv *priv = netdev_priv(dev);
	return &priv->stats;
}

static int zn_config(struct net_device *dev, struct ifmap *map)
{
	printk("%s called\n", __func__);
	return 0;
}

const struct net_device_ops zn_netdev_ops = {
	.ndo_open = zn_open,
	.ndo_stop = zn_close,
	.ndo_set_config = zn_config,
	.ndo_start_xmit = zn_hard_start_xmit,
	.ndo_get_stats = zn_stats,
	.ndo_tx_timeout = zn_tx_timeout,
	.ndo_set_mac_address = zn_set_mac_address,
};

const struct header_ops zn_header_ops = {
	.create = zn_header,
	.rebuild = zn_rebuild_header,
	.cache = NULL,
};
