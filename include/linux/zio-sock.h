#include <linux/socket.h>

#define ETH_P_ZIO		0x5678
#define NET_ZIO_ALIGN		2

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

#define ZSOCK_CONNECTED		0x1
#define ZSOCK_BOUND		0x2
#define ZSOCK_SENDTO		0x4
#define ZSOCK_DEV_BOUND		0x8
#define ZSOCK_CSET_BOUND	0x10
/*Test option flag to use with set/getsockopt*/
#define ZSOCK_OPT1		0x20
#define ZSOCK_OPT2		0x40


extern struct zio_object_list_item *zio_find_dev_by_name(char *name);

/*==Struct area==*/

struct zio_dest {
	uint32_t dev_id;
	char devname[ZIO_OBJ_NAME_LEN];
	struct zio_channel *chan;
	struct zio_cset *cset;
	struct zio_bi *bi;
	struct zio_ti *ti;
};

struct zio_sock {
	struct sock sk;
	/*Add protocol specific member here*/
	struct zio_dest connected_chan;
	struct zio_dest sendto_chan;
	struct zio_block *active_block;
	struct zio_block *out_block;
	uint32_t flags;
	struct list_head list;
};

/*Struct to be placed inside sk_buff->cb field*/
struct zio_cb {
	/*The first byte of skb->cb are written by someone else, so we take
	 * a little padding to be secure our data remain untouched*/
	char unused[sizeof(((struct sk_buff *)0)->cb)
			- sizeof(struct zio_sock *) - sizeof(struct zio_block *)
			- sizeof(int)];
	struct zio_sock *zsk;
	struct zio_block *block;
	int flags;
};

struct zn_priv {
	/*Stats-related fields*/
	struct net_device_stats stats;
	struct net_device *netdev;
};

extern const struct net_proto_family zn_protocol_family;
extern struct packet_type zn_packet;
extern const struct net_device_ops zn_netdev_ops;
extern const struct header_ops zn_header_ops;
extern struct net_device *get_output_device(struct zio_addr *zaddr);
extern struct net_device *netdev;



#define zio_sk(__sk) ((struct zio_sock *)__sk)

struct zn_instance {
	struct zio_bi bi;
	int nitem;
	struct list_head list;
};

#define to_zni(bi) container_of(bi, struct zn_instance, bi)


struct zn_item {
	struct zio_block block;
	struct list_head list;
	struct zn_instance *instance;
	struct sk_buff *skb;
};

#define to_item(block) container_of(block, struct zn_item, block)

extern struct list_head zn_sock_list;

#endif /* __KERNEL__ */
