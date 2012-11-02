#ifndef __ZIO_SOCK_H__
#define __ZIO_SOCK_H__

#include <linux/socket.h>

/* The following two are temporary choices */
#define ETH_P_ZIO		0x5678
#define PF_ZIO			28
#define AF_ZIO			PF_ZIO

#ifndef __KERNEL__ /* For user-space, define sockaddr_zio canonically */
#include <sys/socket.h>
#include <stdint.h>

struct sockaddr_zio {
	sa_family_t sa_family;
	uint8_t host_type;	/* 0 == local, 1 == MAC, ... */
	uint8_t filler;
	uint8_t hostid[8];	/* MAC or other info */
	uint32_t dev_id;	/* Driver-specific id */
	uint16_t cset;		/* index of channel-set within device */
	uint16_t chan;		/* index of channel within cset */
	char devname[ZIO_OBJ_NAME_LEN];
};

#else /* __KERNEL__ */

#include <net/sock.h>

#define ZN_SOCK_CONNECTED	0x1
#define ZN_SOCK_BOUND		0x2
#define ZN_SOCK_SENDTO		0x4
#define ZN_SOCK_DEV_BOUND	0x8
#define ZN_SOCK_CSET_BOUND	0x10

#define NET_ZIO_ALIGN		2

/* FIXME: this is only used in sock-syscall.c */
struct zn_dest {
	uint32_t dev_id;
	char devname[ZIO_OBJ_NAME_LEN];
	struct zio_channel *chan;
	struct zio_cset *cset;
	struct zio_bi *bi;
	struct zio_ti *ti;
};

struct zn_sock {
	struct sock sk;
	/*Add protocol specific member here*/
	struct zn_dest connected_chan;
	struct zn_dest sendto_chan;
	struct zio_block *active_block;
	struct zio_block *out_block;
	uint32_t flags;
	struct list_head list;
};

/* Structure to be placed inside sk_buff->cb field */
struct __zn_cb {
	struct zn_sock *zsk;
	struct zio_block *block;
	int flags;
};

struct zn_cb {
	/*
	 * The leading part of skb->cb is already in use, it seems.
	 * So place our stuff at the end (FIXME: check this).
	 * Unfortunately cb doesn't have a name, so we need this hack
	 */
	char unused[sizeof(((struct sk_buff *)0)->cb)
		    - sizeof(struct __zn_cb)];
	struct __zn_cb zcb;
};

/* Stats and nothing more, by now */
struct zn_priv {
	struct net_device_stats stats;
};

extern const struct net_proto_family zn_protocol_family;
extern struct packet_type zn_packet;
extern const struct net_device_ops zn_netdev_ops;
extern const struct header_ops zn_header_ops;
extern struct net_device *zn_get_output_device(struct zio_addr *zaddr);
extern struct net_device *zn_netdev;

#define zn_sk(__sk) ((struct zn_sock *)__sk)

/* This is the buffer instance (one per channel) */
struct zn_instance {
	struct zio_bi bi;
	int nitem;
	struct list_head list;
};

#define to_zni(bi) container_of(bi, struct zn_instance, bi)

/* And this is the buffer list item (one per block) */
struct zn_item {
	struct zio_block block;
	struct list_head list;
	struct zn_instance *instance;
	struct sk_buff *skb;
};

#define to_zn_item(block) container_of(block, struct zn_item, block)

/* All open sockets */
extern struct list_head zn_sock_list;

#endif /* __KERNEL__ */
#endif /* __ZIO_SOCK_H__ */
