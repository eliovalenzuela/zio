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
#include <linux/device.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/tcp_states.h> /* TCP_ESABLISHED is needed for SOCK_STREAM */

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-user.h>
#include <linux/zio-sock.h>


static struct proto zn_proto = {
	.name = "ZIO",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct zio_sock),
};

static DEFINE_SPINLOCK(zn_list_lock);

static int zn_getname(struct socket *sock, struct sockaddr *uaddr,
						int *uaddr_len, int peer)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct zio_addr *szio = (void *)uaddr;
	struct zio_dest *d = &zsk->connected_chan;

	if (!((zsk->flags & ZSOCK_CONNECTED) || (zsk->flags & ZSOCK_BOUND)))
		return -ENOTCONN;

	szio->sa_family = AF_ZIO;
	szio->dev_id = d->dev_id;

	if (zsk->flags & ZSOCK_DEV_BOUND){
		szio->cset = PFZIO_BIND_ANY;
		goto out;
	}

	szio->cset = d->cset->index;

	if (zsk->flags & ZSOCK_CSET_BOUND){
		szio->chan = PFZIO_BIND_ANY;
		goto out;
	}

	szio->chan = d->chan->index;

out:	szio->host_type = 0;			/*Not used atm, set to zero*/
	memset(szio->hostid, 0, sizeof(szio->hostid));
	strcpy(szio->devname, d->cset->zdev->head.name);

	*uaddr_len = sizeof(struct zio_addr);

	return 0;
}

static unsigned int zn_poll(struct file *file, struct socket *sock,
							poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	int mask = 0;

	if (!((zsk->flags & ZSOCK_CONNECTED) || (zsk->flags & ZSOCK_BOUND)))
		return -ENOTCONN;

	sock_poll_wait(file, sk_sleep(sk), wait);

	/* exceptional events? */
	if (sk->sk_err)
		mask |= POLLERR;
	if (sk->sk_shutdown == SHUTDOWN_MASK)
		mask |= POLLHUP;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		mask |= POLLRDHUP | POLLIN | POLLRDNORM;

	/* readable? */
	if (!skb_queue_empty(&sk->sk_receive_queue))
		mask |= POLLIN | POLLRDNORM;

	/*TODO writetable?*/
	mask |= POLLOUT | POLLWRNORM | POLLWRBAND;

	return mask;
}

static void zn_copy_addr(struct msghdr *msg, struct zio_block *block)
{
	struct zio_addr *szio = (void *)msg->msg_name;
	struct zio_control *ctrl = zio_get_ctrl(block);

	if (msg->msg_namelen < sizeof(struct zio_addr))
		return;

	msg->msg_namelen = sizeof(struct zio_addr);
	memcpy(szio, &ctrl->addr, sizeof(struct zio_addr));
	szio->sa_family = AF_ZIO;

	/*TODO: Add something to set szio->host_type*/

	return;
}

/*Retrieves an skb from the input queue and a block pointer from it*/
static int __zn_get_skb_block(struct zio_sock *zsk, struct zio_block **blockptr,
				int flags)
{
	struct zio_cb *cb;
	struct sk_buff *skb;
	int err;

	skb = skb_recv_datagram((struct sock *)zsk, 0,
						flags & MSG_DONTWAIT, &err);
	if (!skb)
		return err;
	cb = (struct zio_cb *)(skb->cb);
	zsk->active_block = cb->zcb.block;
	*blockptr = zsk->active_block;
	return 0;
}

static int zn_recvmsg_dgram(struct kiocb *iocb, struct socket *sock,
			struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct zio_dest *d = &zsk->connected_chan;
	struct zio_block *block = zsk->active_block;
	struct zn_item *item;
	int err = 0;

	if (unlikely(!(zsk->flags & (ZSOCK_CONNECTED | ZSOCK_BOUND))))
		return -ENOTCONN;

	err = __zn_get_skb_block(zsk, &block, flags);
	if (err)
		return err;

	if (size > block->datalen - block->uoff)
		size = block->datalen - block->uoff;

	err = memcpy_toiovec(msg->msg_iov,
		block->data + block->uoff, size);

	if (unlikely(err))
		return err;

	if (flags & MSG_PEEK)
		return size;

	if (flags & MSG_TRUNC) {
		size = block->datalen;
	}

	if (msg->msg_name && msg->msg_namelen)
		zn_copy_addr(msg, block);

	if (zsk->flags & ZSOCK_BOUND){
	/*At bound time we can't know where this block will come from.
	 * So we discover the associated buffer here. For connected socket the
	 * buffer istance is known and saved at connect time*/
		item = to_item(block);
		d->bi = &item->instance->bi;
	}

	d->bi->b_op->free_block(d->bi, block);
	zsk->active_block = NULL;

	return size;
}

static int zn_recvmsg_stream(struct kiocb *iocb, struct socket *sock,
			struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct zio_dest *d = &zsk->connected_chan;
	struct zio_block *block = zsk->active_block;
	struct zn_item *item;
	int err = 0, copied = 0, left;

	if (unlikely(!(zsk->flags & (ZSOCK_CONNECTED | ZSOCK_BOUND))))
		return -ENOTCONN;

	while (size > copied) {
		if ((block = zsk->active_block) == NULL) {		/*FIXME Likely?*/
			err = __zn_get_skb_block(zsk, &block, flags);
			if (err)
				return err;
		}

		if (copied + block->datalen - block->uoff > size) {
			left = size - copied;
			if (memcpy_toiovec(msg->msg_iov, block->data
							+ block->uoff, left))
				return -EFAULT;
			if (!(flags & MSG_PEEK))
				block->uoff += left;
			if (msg->msg_name && msg->msg_namelen)
				zn_copy_addr(msg, block);
			return size;
		}

		if (memcpy_toiovec(msg->msg_iov, block->data + block->uoff,
						block->datalen - block->uoff))
			return -EFAULT;
		copied += block->datalen - block->uoff;

		if (flags & MSG_PEEK)
			return copied;

		block->uoff += block->datalen - block->uoff;
		if (unlikely(size == copied))
			if (msg->msg_name && msg->msg_namelen)
				zn_copy_addr(msg, block);
		if (block->datalen == block->uoff) {
			if (zsk->flags & ZSOCK_BOUND){
				item = to_item(block);
				d->bi = &item->instance->bi;
			}
			d->bi->b_op->free_block(d->bi, block);
			zsk->active_block = NULL;
		}
	}

	return copied;

}

static int zn_recvmsg_raw(struct kiocb *iocb, struct socket *sock,
			struct msghdr *msg, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct zio_dest *d = &zsk->connected_chan;
	struct zio_control *ctrl;
	struct zio_block *block = zsk->active_block;
	struct zn_item *item;
	int err;

	if (unlikely(!(zsk->flags & (ZSOCK_CONNECTED | ZSOCK_BOUND))))
		return -ENOTCONN;

	err = __zn_get_skb_block(zsk, &block, flags);
	if (err)
		return err;

	if (size > ZIO_CONTROL_SIZE + block->datalen)
		size = ZIO_CONTROL_SIZE + block->datalen;
	ctrl = zio_get_ctrl(block);
	if (memcpy_toiovec(msg->msg_iov, (unsigned char *)ctrl,
						ZIO_CONTROL_SIZE))
		return -EFAULT;
	if (memcpy_toiovec(msg->msg_iov, block->data + block->uoff,
					size - ZIO_CONTROL_SIZE))
		return -EFAULT;

	if (flags & MSG_PEEK) {
		return block->datalen + ZIO_CONTROL_SIZE;
	}

	if (flags & MSG_TRUNC) {
		size = block->datalen + ZIO_CONTROL_SIZE;
	}

	if (msg->msg_name && msg->msg_namelen)
		zn_copy_addr(msg, block);

	if (zsk->flags & ZSOCK_BOUND){
		item = to_item(block);
		d->bi = &item->instance->bi;
	}

	d->bi->b_op->free_block(d->bi, block);
	zsk->active_block = NULL;

	return size;
}

static int __zn_resolve(struct zio_addr *zaddr, struct zio_dest *d,
					struct zio_sock *zsk)
{
	struct zio_device *ziodev;

	ziodev = zio_find_device(zaddr->devname, zaddr->dev_id);
	if (ziodev == NULL) {
		pr_debug("ZIO - Can't find registered device\n");
		return -ENODEV;
	}
	d->dev_id = zaddr->dev_id;
	/*TODO How can i use dev_id from my sockaddr_zio?*/
	strncpy(d->devname, ziodev->head.name, ZIO_OBJ_NAME_LEN);

	if (zaddr->cset == PFZIO_BIND_ANY){
		zsk->flags |= ZSOCK_DEV_BOUND | ZSOCK_BOUND;
		return 0;
	}

	if (!(zaddr->cset <= ziodev->n_cset - 1)) {
		pr_debug("ZIO - Cset out of bound!\n");
		return -EINVAL;
	}

	d->cset = &ziodev->cset[zaddr->cset];

	if (zaddr->chan == PFZIO_BIND_ANY){
		zsk->flags |= ZSOCK_CSET_BOUND | ZSOCK_BOUND;
		return 0;
	}

	if (!(zaddr->chan <= d->cset->n_chan - 1)) {
		pr_debug(KERN_ERR "ZIO - Channel out of bound!\n");
		return -EINVAL;
	}

	d->chan = &d->cset->chan[zaddr->chan];
	if (d->chan->flags & ZIO_DISABLED)
		return -ENODEV;
	d->bi = d->chan->bi;
	d->ti = d->cset->ti;

	return 0;
}

/*type is used to tell if working with sock_raw or not*/
static int __zn_get_out_block(struct zio_sock *zsk, struct zio_dest *d,
			struct zio_block **block, struct msghdr *msg, int type)
{
	struct zio_control *ctrl;
	int datalen;

	ctrl = zio_alloc_control(GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	if (type == SOCK_RAW)
		memcpy_fromiovec((unsigned char *)d->chan->current_ctrl,
					msg->msg_iov, ZIO_CONTROL_SIZE);

	memcpy((unsigned char *)ctrl, d->bi->chan->current_ctrl,
							ZIO_CONTROL_SIZE);

	datalen = ctrl->ssize * ctrl->nsamples;

	*block = d->bi->b_op->alloc_block(d->bi, ctrl, datalen, GFP_KERNEL);

	if (IS_ERR(*block))
		return PTR_ERR(block);
	zsk->out_block = *block;

	return 0;
}

static int __zn_prepare_out_block(struct zio_sock *zsk, struct sk_buff **skb,
			struct msghdr *msg, struct zio_block *block, int len)
{
	struct zn_item *item;
	struct zio_cb *cb;

	item = to_item(block);
	*skb = item->skb;
	/*this skb_set_owner is useless atm. In hard_txmit the field skb->sk is
	null, so useless, dont know why...*/
	skb_set_owner_w(*skb, (struct sock *)zsk);
	/*skb->sk = (struct sock *)zsk;*/
	/*So i try with this ugly thing for now*/
	cb = (struct zio_cb *)(*skb)->cb;
	cb->zcb.zsk = zsk;

	if (msg->msg_name)
		cb->zcb.flags |= ZSOCK_SENDTO;
	else
		cb->zcb.flags &= ~ZSOCK_SENDTO;

	(*skb)->dev = zn_netdev;
	(*skb)->pkt_type = PACKET_HOST;

	len = min_t(size_t, block->datalen - block->uoff, len);

	if (memcpy_fromiovec(block->data + block->uoff, msg->msg_iov, len))
		return -EFAULT;

	block->uoff += len;
	return len;
}

static int __zn_xmit_block(struct zio_sock *zsk, struct zio_block *block,
							struct sk_buff *skb)
{
	int err;

	err = dev_queue_xmit(skb);

	if (err > 0) {
		err = net_xmit_errno(err);
		return err;
	}

	zsk->out_block = NULL;

	return 0;
}

static int zn_sendmsg_dgram(struct kiocb *kiocb, struct socket *sock,
			struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct zio_dest *d = &zsk->connected_chan;
	struct sk_buff *skb;
	struct zio_block *block = zsk->out_block;
	int err;

	printk("%s called\n", __func__);

	if (msg->msg_name) {
		err = __zn_resolve((struct zio_addr *)msg->msg_name,
					&zsk->sendto_chan, zsk);
		if (err)
			return err;
		d = &zsk->sendto_chan;
	} else {
		if (unlikely(!(zsk->flags & ZSOCK_CONNECTED)))
			return -ENOTCONN;
	}

	if (unlikely((d->bi->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		return -EOPNOTSUPP;

	/*Check if we have a block ready to send data*/
	if (block == NULL) {
		err = __zn_get_out_block(zsk, d, &block, msg, 0);
		if (err)
			return err;
	}

	len = __zn_prepare_out_block(zsk, &skb, msg, block, len);
	if (len < 0)
		return len;

	if (block->datalen == block->uoff)
		err = __zn_xmit_block(zsk, block, skb);
	if (err)
		return err;

	return len;
}

static int zn_sendmsg_stream(struct kiocb *kiocb, struct socket *sock,
			struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct zio_dest *d = &zsk->connected_chan;
	struct sk_buff *skb;
	struct zio_block *block = zsk->out_block;
	int err, pushed = 0, tot_pushed = 0;

	if (msg->msg_name) {
		err = __zn_resolve((struct zio_addr *)msg->msg_name,
						&zsk->sendto_chan, zsk);
		if (err)
			return err;
		d = &zsk->sendto_chan;
	} else {
		if (unlikely(!(zsk->flags & ZSOCK_CONNECTED)))
			return -ENOTCONN;
	}

	if (unlikely((d->bi->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		return -EOPNOTSUPP;

	while (tot_pushed < len) {
		if ((block = zsk->out_block) == NULL) {
			err = __zn_get_out_block(zsk, d, &block, msg,
								SOCK_STREAM);
			if (err)
				return err;
		}

		pushed = __zn_prepare_out_block(zsk, &skb, msg, block,
							len - tot_pushed);
		if (pushed < 0)
			return pushed;

		tot_pushed += pushed;

		if (block->datalen == block->uoff)
			err = __zn_xmit_block(zsk, block, skb);
		if (err)
			return err;
	}

	return tot_pushed;
}

static int zn_sendmsg_raw(struct kiocb *kiocb, struct socket *sock,
			struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct zio_dest *d = &zsk->connected_chan;
	struct sk_buff *skb;
	struct zio_block *block = zsk->out_block;
	int err;

	if (msg->msg_name) {
		err = __zn_resolve((struct zio_addr *)msg->msg_name,
						&zsk->sendto_chan, zsk);
		if (err)
			return err;
		d = &zsk->sendto_chan;
	} else {
		if (unlikely(!(zsk->flags & ZSOCK_CONNECTED)))
			return -ENOTCONN;
	}

	if (unlikely((d->bi->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		return -EOPNOTSUPP;

	/*Check if we have a block ready to send data*/
	if (block == NULL) {
		if (len < ZIO_CONTROL_SIZE)
			return -EINVAL;

		err = __zn_get_out_block(zsk, d, &block, msg, SOCK_RAW);
		if (err)
			return err;
	}

	len = __zn_prepare_out_block(zsk, &skb, msg, block,
							len - ZIO_CONTROL_SIZE);
	if (len < 0)
		return len;

	if (block->datalen == block->uoff)
		err = __zn_xmit_block(zsk, block, skb);
	if (err)
		return err;

	return len;
}

static int zn_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	int ret;

	ret = __zn_resolve((struct zio_addr *)addr, &zsk->connected_chan,
									zsk);
	if (ret < 0)
		return ret;

	sock->state = SS_CONNECTED;
	/*skb_recv_datagram wont work without this one on SOCK_STREAM...*/
	sk->sk_state = TCP_ESTABLISHED;

	return 0;
}

static int zn_connect(struct socket *sock, struct sockaddr *addr,
							int alen, int flags)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	int ret;

	printk("Called %s\n", __func__);
	ret = __zn_resolve((struct zio_addr *)addr, &zsk->connected_chan,
									zsk);
	if (ret < 0)
		return ret;

	zsk->flags |= ZSOCK_CONNECTED;
	sock->state = SS_CONNECTED;
	/*skb_recv_datagram wont work without this one on SOCK_STREAM...*/
	sk->sk_state = TCP_ESTABLISHED;

	return 0;
}

static int zn_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk = zio_sk(sk);
	struct net *net = sock_net(sk);

	if (!sk)
		return 0;

	spin_lock_bh(&net->packet.sklist_lock);
	sk_del_node_init(sk);
	sock_prot_inuse_add(net, sk->sk_prot, -1);
	spin_unlock_bh(&net->packet.sklist_lock);

	/*
	 * Detach socket from process context.
	 * Announce socket dead, detach it from wait queue and inode.
	 */
	sock_orphan(sk);
	sock->sk = NULL;

	/*Remove socket from our socket list*/
	spin_lock(&zn_list_lock);
	list_del(&zsk->list);
	spin_unlock(&zn_list_lock);

	/* Ungrab socket and destroy it, if it was the last reference.*/
	sock_put(sk);

	return 0;
}

static int zn_mmap(struct file *file, struct socket *sock,
						struct vm_area_struct *vma)
{
	printk("Called %s\n", __func__);
	return 0;
}

static const struct proto_ops zn_ops_dgram = {
	.family = PF_ZIO,
	.owner = THIS_MODULE,
	.bind = zn_bind,
	.connect = zn_connect,
	.release = zn_release,
	.getname = zn_getname,
	.poll = zn_poll,
	.recvmsg = zn_recvmsg_dgram,
	.sendmsg = zn_sendmsg_dgram,
	.mmap = zn_mmap,
	/*TODO BE CONTINUED*/
};

static const struct proto_ops zn_ops_stream = {
	.family = PF_ZIO,
	.owner = THIS_MODULE,
	.bind = zn_bind,
	.connect = zn_connect,
	.release = zn_release,
	.getname = zn_getname,
	.poll = zn_poll,
	.recvmsg = zn_recvmsg_stream,
	.sendmsg = zn_sendmsg_stream,
	.mmap = zn_mmap,
	/*TODO BE CONTINUED*/
};

static const struct proto_ops zn_ops_raw = {
	.family = PF_ZIO,
	.owner = THIS_MODULE,
	.bind = zn_bind,
	.connect = zn_connect,
	.release = zn_release,
	.getname = zn_getname,
	.poll = zn_poll,
	.recvmsg = zn_recvmsg_raw,
	.sendmsg = zn_sendmsg_raw,
	.mmap = zn_mmap,
	/*TODO BE CONTINUED*/
};

static int zn_create_sock(struct net *net, struct socket *sock, int protocol,
								int kern)
{
	struct sock *sk = sock->sk;
	struct zio_sock *zsk;

	if (protocol && protocol != PF_ZIO)
		return -EPROTONOSUPPORT;

	sock->state = SS_UNCONNECTED;

	switch (sock->type) {
	case SOCK_DGRAM:
		sock->ops = &zn_ops_dgram;
		break;
	case SOCK_STREAM:
		sock->ops = &zn_ops_stream;
		break;
	case SOCK_RAW:
		sock->ops = &zn_ops_raw;
		break;
	}

	sk = sk_alloc(net, PF_ZIO, GFP_KERNEL, &zn_proto);

	if (!sk) {
		printk(KERN_ERR "ZIO - Error while allocating socket\n");
		return -ENOMEM;
	}

	sock_init_data(sock, sk);
	sk->sk_protocol = protocol;

	/*
	 * Do here protocol specific data init eg. pointer to din.alloc. memory
	 *
	 *This way
	 *
	 * struct zio_sock *zsk;
	 * zsk = zio_sk(sk);
	 * zsk->parametro = valore;
	 */

	zsk = zio_sk(sk);
	zsk->flags = 0;

	/*Add socket to currently open sock list*/
	spin_lock(&zn_list_lock);
	list_add_tail(&zsk->list, &zn_sock_list);
	spin_unlock(&zn_list_lock);

	return 0;
}

const struct net_proto_family zn_protocol_family = {
	.family = PF_ZIO,
	.create = zn_create_sock,
	.owner = THIS_MODULE,
};

/*
 * ======================================================
 * Zio socket ENDS here
 * ======================================================
 */

static struct sock *get_target_sock(struct sk_buff *skb)
{
	struct zio_sock *zsk = NULL;
	struct zio_cb *cb = (struct zio_cb *)skb->cb;
	struct zio_block *block = cb->zcb.block;
	struct zio_control *ctrl = zio_get_ctrl(block);
	struct zio_dest *d;
	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, &zn_sock_list) {
		zsk = list_entry(pos, struct zio_sock, list);
		d = &zsk->connected_chan;
		/*This is needed because a still not-connected socket could be
		 * checked against incoming packet*/
		if (d == NULL)
			return NULL;
		/*TODO dev_id now is never set, but this is needed!*/
		if (ctrl->addr.dev_id != d->dev_id)
			continue;
		if (zsk->flags & ZSOCK_DEV_BOUND)
			return (struct sock *)zsk;
		if (ctrl->addr.cset != d->cset->index)
			continue;
		if ((zsk->flags & ZSOCK_CSET_BOUND) ||
					(ctrl->addr.chan == d->chan->index))
			return (struct sock *)zsk;
	}

	return NULL;
}

static int zn_rcv(struct sk_buff *skb, struct net_device *dev,
			struct packet_type *pkt, struct net_device *orig_dev)
{
	struct zn_priv *priv = netdev_priv(skb->dev);
	struct net_device_stats *stats = &priv->stats;
	struct sock *sk = get_target_sock(skb);

	if (sk == NULL) {
		kfree_skb(skb);
		stats->rx_dropped++;
		return NET_RX_DROP;
	}

	sock_queue_rcv_skb(sk, skb);

	stats->rx_packets++;
	stats->rx_bytes += skb->len;

	return NET_RX_SUCCESS;
}

struct packet_type zn_packet = {
	.type = cpu_to_be16(ETH_P_ZIO),
	.func = zn_rcv,
};

