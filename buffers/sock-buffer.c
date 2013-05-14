/* Simone Nellaga, GNU GPLv2 or later */

/*
 * This a buffer implementation for the PF_ZIO protocol family.
 * It allows users to task with ZIO using sockets. It links with
 * sock-nic (network interface) and sock-syscall (recv/send etc).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/net.h>
#include <net/sock.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>

/* We have one attribute only, and we silently accept the changes */
static ZIO_ATTR_DEFINE_STD(ZIO_BUF, zn_std_zattr) = {
	ZIO_ATTR(zbuf, ZIO_ATTR_ZBUF_MAXLEN, S_IRUGO | S_IWUGO, 0,
		 ZN_DEFAULT_BUFFER_LENGTH),
};
static int zn_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	return 0;
}
struct zio_sysfs_operations zn_sysfs_ops = {
	.conf_set = zn_conf_set,
};

/* memcache for allocating buffer items */
static struct kmem_cache *zn_block_memcache;

/* Open socket list */
struct list_head zn_sock_list;

/* Called by the socket-layer sendmsg (output) and by the trigger (input) */
static struct zio_block *zn_alloc_block(struct zio_bi *bi,
					struct zio_control *ctrl,
					size_t datalen, gfp_t gfp)
{
	struct zn_instance *zni = to_zni(bi);
	struct zn_item *item;
	struct sk_buff *skb;
	struct zn_cb *cb;
	size_t ctrl_size = zio_control_size(bi->chan);
	void *ptr;
	const int headspace = sizeof(struct ethhdr) + NET_IP_ALIGN
		+ ctrl_size;

	item = kmem_cache_zalloc(zn_block_memcache, gfp);

	/* Alloc skb to hold data going to/coming from the framework*/
	skb = alloc_skb(headspace + datalen, gfp);
	if (!skb || !item) {
		printk("%s: problem allocating skb or item\n", __func__);
		goto out;
	}

	skb_reserve(skb, headspace);
	/*
	 * The trigger will store data without access to the sk_buff, so we
	 * prepare it for him with skb_put
	 */
	item->block.data = skb_put(skb, datalen);

	memcpy(skb_push(skb, ctrl_size), ctrl, ctrl_size);

	/* Build ethernet header (not used by now */
	dev_hard_header(skb, zn_netdev, ETH_P_ZIO, zn_netdev->dev_addr,
			zn_netdev->dev_addr, skb->len);

	skb->protocol = cpu_to_be16(ETH_P_ZIO);

	skb_reset_mac_header(skb);

	item->block.datalen = datalen;
	item->instance = zni;
	item->skb = skb;

	zio_set_ctrl(&item->block, ctrl);

	ptr = &item->block;
	cb = (struct zn_cb *)skb->cb;
	cb->zcb.block = ptr;

	return &item->block;

out:
	kfree_skb(skb);
	kmem_cache_free(zn_block_memcache, item);
	return ERR_PTR(-ENOMEM);
}

/* Called by socket-layer recvmsg (input) or by the trigger (output) */
static void zn_free_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zn_item *item;

	item = to_zn_item(block);
	kfree_skb(item->skb);
	zio_free_control(zio_get_ctrl(block));
	kmem_cache_free(zn_block_memcache, item);
}

/* Called by the trigger (input) or by ndo_hard_xmit (output) */
static int zn_store_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zn_instance *zni = to_zni(bi);
	struct zn_item *item = to_zn_item(block);
	struct sk_buff *skb;
	int ret;

	if (unlikely(!zio_get_ctrl(block))) {
		WARN_ON(1);
		return -EINVAL;
	}

	/* We take blocks and enqueue them to the network subsystem */
	if ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT) {
		/*
		 * When the trigger tries to store_block, we don't
		 * save it in the buffer but exploit the netif_rx
		 * queue to send block to the user
		 */
		skb = item->skb;
		skb->protocol = cpu_to_be16(ETH_P_ZIO);
		skb->pkt_type = PACKET_HOST;
		skb->dev = zn_netdev;
		ret = netif_rx_ni(skb);
		return ret;
	}

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT) {
		spin_lock(&bi->lock);
		if (zni->nitem >=
			bi->zattr_set.std_zattr[ZIO_ATTR_ZBUF_MAXLEN].value) {
			goto out_unlock;
		}
		zni->nitem++;
		list_add_tail(&item->list, &zni->list);
		spin_unlock(&bi->lock);
		return 0;
	}

	return -EINVAL;
out_unlock:
	spin_unlock(&bi->lock);
	return -ENOSPC;
}

/* Called by the trigger (output) */
static struct zio_block *zn_retr_block(struct zio_bi *bi)
{
	struct zn_instance *zni;
	struct zn_item *item;

	zni = to_zni(bi);

	spin_lock(&bi->lock);
	if (list_empty(&zni->list)){
		spin_unlock(&bi->lock);
		return NULL;
	}
	item = list_first_entry(&zni->list, struct zn_item, list);
	list_del(&item->list);
	zni->nitem--;
	spin_unlock(&bi->lock);

	return &item->block;

}

/* Called by zio for each channel electing to use this buffer type */
static struct zio_bi *zn_create(struct zio_buffer_type *zbuf,
				 struct zio_channel *chan)
{
	struct zn_instance *zni;

	zni = kzalloc(sizeof(*zni), GFP_KERNEL);
	if (!zni)
		return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&zni->list);

	return &zni->bi;
}

/* Called by zio on channel removal or if it changes buffer type */
static void zn_destroy(struct zio_bi *bi)
{
	struct zn_instance *zni = to_zni(bi);
	struct zn_item *item;
	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, &zni->list) {
		item = list_entry(pos, struct zn_item, list);
		zn_free_block(&zni->bi, &item->block);
	}
	kfree(zni);
}

static const struct zio_buffer_operations zn_buffer_ops = {
	.alloc_block =	zn_alloc_block,
	.free_block =	zn_free_block,
	.store_block =	zn_store_block,
	.retr_block =	zn_retr_block,
	.create =	zn_create,
	.destroy =	zn_destroy,
};

static struct zio_buffer_type zn_buffer = {
	.owner =	THIS_MODULE,
	.zattr_set = {
		.std_zattr = zn_std_zattr,
	},
	.s_op = &zn_sysfs_ops,
	.b_op = &zn_buffer_ops,
	/*.f_op = &zio_generic_file_operations, TODO, should enable these? */
};

static int __init zn_init(void)
{
	struct zn_priv *priv;
	int ret;

	sock_register(&zn_protocol_family);
	dev_add_pack(&zn_packet);
	zn_netdev = alloc_etherdev(sizeof(struct zn_priv));
	if (!zn_netdev) {
		dev_remove_pack(&zn_packet);
		sock_unregister(PF_ZIO);
		return -ENODEV; /* FIXME */
	}
	strcpy(zn_netdev->name, "zio");
	zn_netdev->netdev_ops = &zn_netdev_ops;
	zn_netdev->header_ops = &zn_header_ops;
	random_ether_addr(zn_netdev->dev_addr);

	priv = netdev_priv(zn_netdev);
	register_netdev(zn_netdev);

	/* Register memcache for the buffer */
	zn_block_memcache = kmem_cache_create("zio-sock",
					      sizeof(struct zn_item),
					      __alignof__(struct zn_item),
					      0, NULL);
	if (!zn_block_memcache) {
		unregister_netdev(zn_netdev);
		free_netdev(zn_netdev);
		dev_remove_pack(&zn_packet);
		sock_unregister(PF_ZIO);
		return -ENOMEM;
	}

	ret = zio_register_buf(&zn_buffer, "socket");

	INIT_LIST_HEAD(&zn_sock_list);
	return ret;
}

static void __exit zn_exit(void)
{
	/* FIXME: the order is wrong */
	unregister_netdev(zn_netdev);
	free_netdev(zn_netdev);
	dev_remove_pack(&zn_packet);
	sock_unregister(PF_ZIO);
	zio_unregister_buf(&zn_buffer);
}

module_init(zn_init);
module_exit(zn_exit);
MODULE_AUTHOR("Simone Nellaga");
MODULE_LICENSE("GPL");
