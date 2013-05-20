/* Simone Nellaga, GNU GPLv2 or later */

/*
 *This a buffer implementation for the new PF_ZIO protocol family, allowing
 * users to exchange data with the framework using sockets. So it implements a
 * new software network interface, and a new socket-layer logic.
 */
#define DEBUG
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/tcp_states.h> /* TCP_ESABLISHED is needed for SOCK_STREAM */

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-user.h>
#include <linux/zio-trigger.h>
#include <linux/zio-sock.h>

struct net_device *zn_netdev;

static int zn_open(struct net_device *dev)
{
	dev_dbg(&dev->dev, "%s called\n", __func__);
	netif_start_queue(dev);
	return 0;
}

static int zn_close(struct net_device *dev)
{
	dev_dbg(&dev->dev, "%s called\n", __func__);
	netif_stop_queue(dev);
	return 0;
}

static int zn_hard_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct zn_priv *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;
	struct zn_sock *zsk;
	struct zn_dest *d;
	struct zio_block *block;
	struct zn_cb *cb = (struct zn_cb *)skb->cb;

	dev_dbg(&dev->dev, "%s called\n", __func__);

	if (unlikely(skb->protocol != cpu_to_be16(ETH_P_ZIO))) {
		/*FIXME kfree_skb needed?*/
		return NET_XMIT_DROP;
	}

	/*Ugly solution to the problem exposed in zn_sendmsg*/
	zsk = cb->zcb.zsk;
	block = cb->zcb.block;

	if (cb->zcb.flags & ZN_SOCK_SENDTO)
		d = &zsk->sendto_chan;
	else
		d = &zsk->connected_chan;

	if (!zio_trigger_try_push(d->bi, d->chan, block))
		zio_buffer_store_block(d->bi, block);

	/*Update stats*/
	stats->tx_packets++;
	stats->tx_bytes += skb->len;
	dev->trans_start = jiffies;
	return NETDEV_TX_OK;
}

static int zn_set_mac_address(struct net_device *dev, void *vaddr)
{
	dev_dbg(&dev->dev, "%s called\n", __func__);
	return 0;
}

/**
 * zn_header stands for the device->hard_header. This function is called before
 * transmission and should build the header for the packet
 */
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
	pr_debug("%s called\n", __func__);
	return 0;
}

static void zn_tx_timeout(struct net_device *dev)
{
	dev_dbg(&dev->dev, "%s called\n", __func__);
	return;
}

struct net_device_stats *zn_stats(struct net_device *dev)
{
	struct zn_priv *priv = netdev_priv(dev);
	return &priv->stats;
}

static int zn_config(struct net_device *dev, struct ifmap *map)
{
	dev_dbg(&dev->dev, "%s called\n", __func__);
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
