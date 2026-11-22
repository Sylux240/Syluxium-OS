// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: network devices, netdevice notifiers, NAPI, the rtnl lock and
 * Ethernet helpers.
 *
 * A registered net_device becomes a Syluxium net::Interface through the
 * bridge in net.cpp (lkpi_sylux_netif_*). Outgoing frames come back as
 * lkpi_netdev_xmit() and go to ndo_start_xmit(); "ip link set X up/down"
 * arrives as lkpi_netdev_set_up(). Received skbs are queued and handed to
 * the Syluxium stack by the "lkpi-netrx" thread, so drivers and mac80211 may
 * call netif_receive_skb() from atomic context (RCU sections, spinlocks).
 */
#include <linux/lkpi_all.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/mii.h>
#include <linux/crc32.h>
#include <linux/kthread.h>
#include <linux/if_arp.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

/* ---- Syluxium side (net.cpp) ---- */
void* lkpi_sylux_netif_register(struct net_device* dev, const char* name, const u8* mac, unsigned int mtu, void* hw,
				int wireless, int up, int* ifindex);
void lkpi_sylux_netif_unregister(void* iface);
void lkpi_sylux_netif_link(void* iface, int carrier);
void lkpi_sylux_netif_flags(void* iface, int up);
void lkpi_sylux_netif_mac(void* iface, const u8* mac);
void lkpi_sylux_netif_rx(void* iface, const u8* frame, size_t len);
int lkpi_sylux_name_taken(const char* name);
int lkpi_log_line(const char* level, const char* text);

/* ---- notifier chains ---- */

int lkpi_notifier_chain_register(struct notifier_block** nl, struct notifier_block* n)
{
	while (*nl) {
		if (n->priority > (*nl)->priority)
			break;
		nl = &(*nl)->next;
	}
	n->next = *nl;
	*nl = n;
	return 0;
}

int lkpi_notifier_chain_unregister(struct notifier_block** nl, struct notifier_block* n)
{
	while (*nl) {
		if (*nl == n) {
			*nl = n->next;
			return 0;
		}
		nl = &(*nl)->next;
	}
	return -ENOENT;
}

int lkpi_notifier_call_chain(struct notifier_block** nl, unsigned long val, void* v)
{
	int ret = NOTIFY_DONE;
	struct notifier_block* nb = *nl;
	while (nb) {
		struct notifier_block* next = nb->next;
		ret = nb->notifier_call(nb, val, v);
		if (ret & NOTIFY_STOP_MASK)
			break;
		nb = next;
	}
	return ret;
}

/* ---- namespaces (exactly one) ---- */

struct net init_net;
const int net_ns_type_operations = 0;
static LIST_HEAD(pernet_list);
static void* pernet_data[16];

int register_pernet_device(struct pernet_operations* ops)
{
	if (ops->id && ops->size) {
		static unsigned int next_id = 1;
		*ops->id = next_id++;
		if (*ops->id >= ARRAY_SIZE(pernet_data))
			return -ENOSPC;
		pernet_data[*ops->id] = kzalloc(ops->size, GFP_KERNEL);
		if (!pernet_data[*ops->id])
			return -ENOMEM;
	}
	list_add_tail(&ops->list, &pernet_list);
	return ops->init ? ops->init(&init_net) : 0;
}

void unregister_pernet_device(struct pernet_operations* ops)
{
	list_del(&ops->list);
	if (ops->pre_exit)
		ops->pre_exit(&init_net);
	if (ops->exit)
		ops->exit(&init_net);
}

void* net_generic(const struct net* net, unsigned int id)
{
	return id < ARRAY_SIZE(pernet_data) ? pernet_data[id] : NULL;
}

/* ---- rtnl ---- */

static DEFINE_MUTEX(rtnl_mutex);
void rtnl_lock(void) { mutex_lock(&rtnl_mutex); }
void rtnl_unlock(void) { mutex_unlock(&rtnl_mutex); }
int rtnl_trylock(void) { return mutex_trylock(&rtnl_mutex); }
int rtnl_is_locked(void) { return mutex_is_locked(&rtnl_mutex); }

/* ---- netdevice notifiers ---- */

static RAW_NOTIFIER_HEAD(netdev_chain);
static LIST_HEAD(netdev_list); /* all registered LinuxKPI net_devices (rtnl) */

int call_netdevice_notifiers(unsigned long val, struct net_device* dev)
{
	struct netdev_notifier_info info = {.dev = dev};
	return raw_notifier_call_chain(&netdev_chain, val, &info);
}

int register_netdevice_notifier(struct notifier_block* nb)
{
	struct net_device* dev;
	rtnl_lock();
	raw_notifier_chain_register(&netdev_chain, nb);
	/* Replay registration (and up state) of existing devices, as Linux does. */
	list_for_each_entry(dev, &netdev_list, dev_list) {
		struct netdev_notifier_info info = {.dev = dev};
		nb->notifier_call(nb, NETDEV_REGISTER, &info);
		if (dev->flags & IFF_UP)
			nb->notifier_call(nb, NETDEV_UP, &info);
	}
	rtnl_unlock();
	return 0;
}

int unregister_netdevice_notifier(struct notifier_block* nb)
{
	rtnl_lock();
	raw_notifier_chain_unregister(&netdev_chain, nb);
	rtnl_unlock();
	return 0;
}

void netdev_state_change(struct net_device* dev)
{
	if (dev->flags & IFF_UP)
		call_netdevice_notifiers(NETDEV_CHANGE, dev);
}

void netdev_features_change(struct net_device* dev)
{
	call_netdevice_notifiers(NETDEV_FEAT_CHANGE, dev);
}

/* ---- allocation ---- */

void ether_setup(struct net_device* dev)
{
	dev->type = ARPHRD_ETHER;
	dev->hard_header_len = ETH_HLEN;
	dev->min_header_len = ETH_HLEN;
	dev->mtu = ETH_DATA_LEN;
	dev->min_mtu = ETH_MIN_MTU;
	dev->max_mtu = ETH_DATA_LEN;
	dev->addr_len = ETH_ALEN;
	dev->tx_queue_len = 1000;
	dev->flags = IFF_BROADCAST | IFF_MULTICAST;
	dev->priv_flags |= IFF_TX_SKB_SHARING;
	eth_broadcast_addr(dev->broadcast);
}

void __hw_addr_init(struct netdev_hw_addr_list* list)
{
	INIT_LIST_HEAD(&list->list);
	list->count = 0;
}

struct net_device* alloc_netdev_mqs(int sizeof_priv, const char* name, unsigned char name_assign_type,
				    void (*setup)(struct net_device*), unsigned int txqs, unsigned int rxqs)
{
	struct net_device* dev;
	unsigned int i;

	if (txqs < 1)
		txqs = 1;
	dev = kzalloc(sizeof(*dev) + sizeof_priv, GFP_KERNEL);
	if (!dev)
		return NULL;
	dev->_tx = kcalloc(txqs, sizeof(*dev->_tx), GFP_KERNEL);
	dev->ethtool = kzalloc(sizeof(*dev->ethtool), GFP_KERNEL);
	if (!dev->_tx || !dev->ethtool) {
		kfree(dev->_tx);
		kfree(dev->ethtool);
		kfree(dev);
		return NULL;
	}
	for (i = 0; i < txqs; i++)
		dev->_tx[i].dev = dev;
	dev->num_tx_queues = dev->real_num_tx_queues = txqs;
	dev->num_rx_queues = dev->real_num_rx_queues = rxqs;
	strscpy(dev->name, name, sizeof(dev->name));
	dev->name_assign_type = name_assign_type;
	dev->dev_addr = dev->lkpi_addr;
	dev->watchdog_timeo = 5000;
	dev->lkpi_priv_size = sizeof_priv;
	dev->lkpi_carrier = 1;
	dev->nd_net = &init_net;
	dev->gso_partial_features = 0;
	refcount_set(&dev->dev_refcnt, 1);
	INIT_LIST_HEAD(&dev->dev_list);
	__hw_addr_init(&dev->uc);
	__hw_addr_init(&dev->mc);
	__hw_addr_init(&dev->dev_addrs);
	if (setup)
		setup(dev);
	return dev;
}

struct net_device* alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs, unsigned int rxqs)
{
	struct net_device* dev = alloc_netdev_mqs(sizeof_priv, "eth%d", NET_NAME_ENUM, ether_setup, txqs, rxqs);
	if (dev)
		dev->lkpi_auto_open = 1; /* Syluxium's Ethernet interfaces are up from boot */
	return dev;
}

void free_netdev(struct net_device* dev)
{
	if (!dev)
		return;
	if (dev->reg_state == NETREG_REGISTERED) {
		/* Freed later by unregister (needs_free_netdev semantics). */
		dev->needs_free_netdev = true;
		return;
	}
	kfree(dev->tstats);
	kfree(dev->_tx);
	kfree(dev->ethtool);
	kfree(dev);
}

/* ---- naming ---- */

static int name_taken(const char* name)
{
	struct net_device* d;
	list_for_each_entry(d, &netdev_list, dev_list)
		if (!strcmp(d->name, name))
			return 1;
	return lkpi_sylux_name_taken(name);
}

int dev_alloc_name(struct net_device* dev, const char* name)
{
	const char* p = strchr(name, '%');
	char buf[IFNAMSIZ];
	int i;

	if (!p) {
		if (name_taken(name))
			return -EEXIST;
		strscpy(dev->name, name, sizeof(dev->name));
		return 0;
	}
	for (i = 0; i < 100; i++) {
		snprintf(buf, sizeof(buf), name, i);
		if (!name_taken(buf)) {
			strscpy(dev->name, buf, sizeof(dev->name));
			return i;
		}
	}
	return -ENFILE;
}

/* ---- registration ---- */

int register_netdevice(struct net_device* dev)
{
	int ret;
	char wanted[IFNAMSIZ];

	if (dev->netdev_ops->ndo_init) {
		ret = dev->netdev_ops->ndo_init(dev);
		if (ret)
			return ret;
	}
	ret = notifier_to_errno(call_netdevice_notifiers(NETDEV_POST_INIT, dev));
	if (ret)
		goto err_uninit;
	strscpy(wanted, dev->name[0] ? dev->name : "eth%d", sizeof(wanted));
	ret = dev_alloc_name(dev, wanted);
	if (ret < 0)
		goto err_uninit;
	if (dev->pcpu_stat_type == NETDEV_PCPU_STAT_TSTATS && !dev->tstats) {
		dev->tstats = kzalloc(sizeof(*dev->tstats), GFP_KERNEL);
		if (!dev->tstats) {
			ret = -ENOMEM;
			goto err_uninit;
		}
	}
	memcpy(dev->perm_addr, dev->dev_addr, dev->addr_len);
	if (dev->type == ARPHRD_ETHER) {
		dev->lkpi_iface = lkpi_sylux_netif_register(dev, dev->name, dev->dev_addr, dev->mtu,
							    dev->dev.parent ? dev->dev.parent->lkpi_registry : NULL,
							    dev->ieee80211_ptr != NULL, 0, &dev->ifindex);
		if (!dev->lkpi_iface) {
			ret = -ENOSPC;
			goto err_uninit;
		}
	} else {
		/* Not Ethernet (e.g. a radiotap monitor device): no IP stack on it,
		 * so it isn't a Syluxium interface; it still needs an ifindex. */
		static int next_private_ifindex = 1000;
		dev->ifindex = next_private_ifindex++;
	}
	set_bit(__LINK_STATE_PRESENT, &dev->state);
	dev->reg_state = NETREG_REGISTERED;
	list_add_tail(&dev->dev_list, &netdev_list);
	ret = notifier_to_errno(call_netdevice_notifiers(NETDEV_REGISTER, dev));
	if (ret) {
		unregister_netdevice(dev);
		return ret;
	}
	return 0;

err_uninit:
	if (dev->netdev_ops->ndo_uninit)
		dev->netdev_ops->ndo_uninit(dev);
	return ret;
}

int register_netdev(struct net_device* dev)
{
	int ret;
	rtnl_lock();
	ret = register_netdevice(dev);
	if (!ret && dev->lkpi_auto_open) {
		ret = dev_open(dev, NULL);
		if (ret)
			printk(KERN_WARNING "%s: open failed (%d)\n", dev->name, ret);
		ret = 0;
	}
	rtnl_unlock();
	return ret;
}

static void netdev_free_work(struct work_struct* w);

void unregister_netdevice_queue(struct net_device* dev, struct list_head* head)
{
	if (dev->reg_state != NETREG_REGISTERED)
		return;
	dev_close(dev);
	dev->reg_state = NETREG_UNREGISTERING;
	call_netdevice_notifiers(NETDEV_UNREGISTER, dev);
	list_del_init(&dev->dev_list);
	if (dev->netdev_ops->ndo_uninit)
		dev->netdev_ops->ndo_uninit(dev);
	if (dev->lkpi_iface)
		lkpi_sylux_netif_unregister(dev->lkpi_iface);
	dev->lkpi_iface = NULL;
	dev->reg_state = NETREG_UNREGISTERED;
	synchronize_net();
	if (dev->priv_destructor)
		dev->priv_destructor(dev);
	if (dev->needs_free_netdev)
		free_netdev(dev);
}

void unregister_netdevice_many(struct list_head* head)
{
}

void unregister_netdev(struct net_device* dev)
{
	rtnl_lock();
	unregister_netdevice(dev);
	rtnl_unlock();
}

int dev_change_net_namespace(struct net_device* dev, struct net* net, const char* pat)
{
	return net == &init_net ? 0 : -EOPNOTSUPP;
}

struct net_device* __dev_get_by_index(struct net* net, int ifindex)
{
	struct net_device* dev;
	list_for_each_entry(dev, &netdev_list, dev_list)
		if (dev->ifindex == ifindex)
			return dev;
	return NULL;
}

struct net_device* dev_get_by_index_rcu(struct net* net, int ifindex)
{
	return __dev_get_by_index(net, ifindex);
}

struct net_device* dev_get_by_index(struct net* net, int ifindex)
{
	struct net_device* dev = __dev_get_by_index(net, ifindex);
	dev_hold(dev);
	return dev;
}

struct net_device* __dev_get_by_name(struct net* net, const char* name)
{
	struct net_device* dev;
	list_for_each_entry(dev, &netdev_list, dev_list)
		if (!strcmp(dev->name, name))
			return dev;
	return NULL;
}

struct net_device* dev_get_by_name(struct net* net, const char* name)
{
	struct net_device* dev = __dev_get_by_name(net, name);
	dev_hold(dev);
	return dev;
}

/* ---- up / down ---- */

int dev_open(struct net_device* dev, struct netlink_ext_ack* extack)
{
	int ret;

	if (dev->flags & IFF_UP)
		return 0;
	ret = notifier_to_errno(call_netdevice_notifiers(NETDEV_PRE_UP, dev));
	if (ret)
		return ret;
	set_bit(__LINK_STATE_START, &dev->state);
	if (dev->netdev_ops->ndo_validate_addr)
		ret = dev->netdev_ops->ndo_validate_addr(dev);
	if (!ret && dev->netdev_ops->ndo_open)
		ret = dev->netdev_ops->ndo_open(dev);
	if (ret) {
		clear_bit(__LINK_STATE_START, &dev->state);
		return ret;
	}
	dev->flags |= IFF_UP;
	if (dev->netdev_ops->ndo_set_rx_mode)
		dev->netdev_ops->ndo_set_rx_mode(dev);
	call_netdevice_notifiers(NETDEV_UP, dev);
	if (dev->lkpi_iface) {
		lkpi_sylux_netif_flags(dev->lkpi_iface, 1);
		lkpi_sylux_netif_link(dev->lkpi_iface, dev->lkpi_carrier);
	}
	return 0;
}

void dev_close(struct net_device* dev)
{
	if (!(dev->flags & IFF_UP))
		return;
	call_netdevice_notifiers(NETDEV_GOING_DOWN, dev);
	clear_bit(__LINK_STATE_START, &dev->state);
	synchronize_net();
	if (dev->netdev_ops->ndo_stop)
		dev->netdev_ops->ndo_stop(dev);
	dev->flags &= ~IFF_UP;
	call_netdevice_notifiers(NETDEV_DOWN, dev);
	if (dev->lkpi_iface)
		lkpi_sylux_netif_flags(dev->lkpi_iface, 0);
}

void dev_close_many(struct list_head* head, bool unlink)
{
}

/* Called by the Syluxium stack: SIOCSIFFLAGS / RTM_SETLINK changed IFF_UP. */
long lkpi_netdev_set_up(struct net_device* dev, int up)
{
	int ret = 0;
	rtnl_lock();
	if (up)
		ret = dev_open(dev, NULL);
	else
		dev_close(dev);
	rtnl_unlock();
	return ret;
}

int dev_set_mtu(struct net_device* dev, int mtu)
{
	if (mtu < (int)dev->min_mtu || mtu > (int)dev->max_mtu)
		return -EINVAL;
	if (dev->netdev_ops->ndo_change_mtu)
		return dev->netdev_ops->ndo_change_mtu(dev, mtu);
	dev->mtu = mtu;
	return 0;
}

int dev_set_mac_address(struct net_device* dev, struct sockaddr* sa, struct netlink_ext_ack* extack)
{
	int ret = 0;
	if (dev->netdev_ops->ndo_set_mac_address)
		ret = dev->netdev_ops->ndo_set_mac_address(dev, sa);
	else
		eth_hw_addr_set(dev, (u8*)sa->sa_data);
	if (!ret) {
		if (dev->lkpi_iface)
			lkpi_sylux_netif_mac(dev->lkpi_iface, dev->dev_addr);
		call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
	}
	return ret;
}

/* Called by the Syluxium stack: SIOCSIFHWADDR. */
long lkpi_netdev_set_mac(struct net_device* dev, const u8* mac)
{
	struct sockaddr sa = {.sa_family = ARPHRD_ETHER};
	long ret;
	memcpy(sa.sa_data, mac, ETH_ALEN);
	rtnl_lock();
	ret = dev_set_mac_address(dev, &sa, NULL);
	rtnl_unlock();
	return ret;
}

void netdev_update_features(struct net_device* dev) {}

/* ---- carrier / queues ---- */

void netif_carrier_on(struct net_device* dev)
{
	if (xchg(&dev->lkpi_carrier, 1))
		return;
	clear_bit(__LINK_STATE_NOCARRIER, &dev->state);
	if (dev->lkpi_iface && (dev->flags & IFF_UP))
		lkpi_sylux_netif_link(dev->lkpi_iface, 1);
}

void netif_carrier_off(struct net_device* dev)
{
	if (!xchg(&dev->lkpi_carrier, 0))
		return;
	set_bit(__LINK_STATE_NOCARRIER, &dev->state);
	if (dev->lkpi_iface)
		lkpi_sylux_netif_link(dev->lkpi_iface, 0);
}

void netif_tx_wake_queue(struct netdev_queue* q)
{
	clear_bit(__QUEUE_STATE_DRV_XOFF, &q->state);
}

/* ---- statistics ---- */

void dev_sw_netstats_rx_add(struct net_device* dev, unsigned int len)
{
	if (dev->tstats) {
		u64_stats_inc(&dev->tstats->rx_packets);
		u64_stats_add(&dev->tstats->rx_bytes, len);
	}
}

void dev_sw_netstats_tx_add(struct net_device* dev, unsigned int packets, unsigned int len)
{
	if (dev->tstats) {
		u64_stats_add(&dev->tstats->tx_packets, packets);
		u64_stats_add(&dev->tstats->tx_bytes, len);
	}
}

void dev_fetch_sw_netstats(struct rtnl_link_stats64* s, const struct pcpu_sw_netstats* netstats)
{
	s->rx_packets += u64_stats_read(&netstats->rx_packets);
	s->rx_bytes += u64_stats_read(&netstats->rx_bytes);
	s->tx_packets += u64_stats_read(&netstats->tx_packets);
	s->tx_bytes += u64_stats_read(&netstats->tx_bytes);
}

void dev_get_tstats64(struct net_device* dev, struct rtnl_link_stats64* s)
{
	netdev_stats_to_stats64(s, &dev->stats);
	if (dev->tstats)
		dev_fetch_sw_netstats(s, dev->tstats);
}

/* ---- hardware address lists (multicast filters) ---- */

int __hw_addr_sync(struct netdev_hw_addr_list* to_list, struct netdev_hw_addr_list* from_list, int addr_len)
{
	struct netdev_hw_addr *ha, *n;
	list_for_each_entry(ha, &from_list->list, list) {
		bool found = false;
		list_for_each_entry(n, &to_list->list, list)
			if (!memcmp(n->addr, ha->addr, addr_len))
				found = true;
		if (!found) {
			n = kzalloc(sizeof(*n), GFP_ATOMIC);
			if (!n)
				return -ENOMEM;
			memcpy(n->addr, ha->addr, addr_len);
			n->refcount = 1;
			list_add_tail(&n->list, &to_list->list);
			to_list->count++;
		}
	}
	return 0;
}

void __hw_addr_unsync(struct netdev_hw_addr_list* to_list, struct netdev_hw_addr_list* from_list, int addr_len)
{
	struct netdev_hw_addr *ha, *n, *tmp;
	list_for_each_entry(ha, &from_list->list, list) {
		list_for_each_entry_safe(n, tmp, &to_list->list, list) {
			if (!memcmp(n->addr, ha->addr, addr_len)) {
				list_del(&n->list);
				to_list->count--;
				kfree(n);
			}
		}
	}
}

/* ---- transmit ---- */

static u16 pick_queue(struct net_device* dev, struct sk_buff* skb)
{
	u16 q = 0;
	if (dev->netdev_ops->ndo_select_queue)
		q = dev->netdev_ops->ndo_select_queue(dev, skb, NULL);
	if (q >= dev->real_num_tx_queues)
		q = 0;
	return q;
}

static int xmit_one(struct net_device* dev, struct sk_buff* skb)
{
	netdev_tx_t r;
	u16 q;

	if (!netif_running(dev)) {
		kfree_skb(skb);
		return -ENETDOWN;
	}
	rcu_read_lock();
	q = pick_queue(dev, skb);
	skb_set_queue_mapping(skb, q);
	if (netif_tx_queue_stopped(netdev_get_tx_queue(dev, q))) {
		rcu_read_unlock();
		kfree_skb(skb);
		return -EAGAIN;
	}
	r = dev->netdev_ops->ndo_start_xmit(skb, dev);
	rcu_read_unlock();
	if (r != NETDEV_TX_OK) {
		kfree_skb(skb);
		return -EAGAIN;
	}
	return 0;
}

int dev_queue_xmit(struct sk_buff* skb)
{
	struct net_device* dev = skb->dev;
	if (!dev) {
		kfree_skb(skb);
		return -ENODEV;
	}
	return xmit_one(dev, skb);
}

/* Called by the Syluxium stack for every outgoing Ethernet frame. */
long lkpi_netdev_xmit(struct net_device* dev, const u8* frame, size_t len)
{
	unsigned int headroom = LL_RESERVED_SPACE(dev) + 64;
	struct sk_buff* skb;

	if (len < ETH_HLEN)
		return -EINVAL;
	skb = alloc_skb(headroom + len + dev->needed_tailroom + 32, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, headroom);
	skb_put_data(skb, frame, len);
	skb->dev = dev;
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, ETH_HLEN);
	skb->protocol = ((struct ethhdr*)frame)->h_proto;
	skb->priority = 0;
	skb->pkt_type = PACKET_OUTGOING;
	return xmit_one(dev, skb);
}

/* ---- receive ---- */

static struct sk_buff_head rx_queue;
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);
static bool rx_started;

static void deliver(struct sk_buff* skb)
{
	struct net_device* dev = skb->dev;
	unsigned char* frame = skb_mac_header_was_set(skb) ? skb_mac_header(skb) : skb->data;
	size_t len = skb->tail - frame;
	if (dev && dev->lkpi_iface && len >= ETH_HLEN && netif_running(dev))
		lkpi_sylux_netif_rx(dev->lkpi_iface, frame, len);
	kfree_skb(skb);
}

static int rx_thread(void* arg)
{
	for (;;) {
		struct sk_buff* skb;
		wait_event(rx_wait, !skb_queue_empty_lockless(&rx_queue));
		while ((skb = skb_dequeue(&rx_queue)) != NULL)
			deliver(skb);
	}
	return 0;
}

static void rx_init(void)
{
	skb_queue_head_init(&rx_queue);
	rx_started = true;
	kthread_run(rx_thread, NULL, "lkpi-netrx");
}

int netif_receive_skb(struct sk_buff* skb)
{
	if (!rx_started) {
		kfree_skb(skb);
		return NET_RX_DROP;
	}
	if (skb_queue_len_lockless(&rx_queue) > 2000) {
		kfree_skb(skb);
		return NET_RX_DROP;
	}
	skb_queue_tail(&rx_queue, skb);
	wake_up(&rx_wait);
	return NET_RX_SUCCESS;
}

int netif_rx(struct sk_buff* skb)
{
	return netif_receive_skb(skb);
}

void netif_receive_skb_list(struct list_head* head)
{
	struct sk_buff *skb, *next;
	list_for_each_entry_safe(skb, next, head, list) {
		skb_list_del_init(skb);
		netif_receive_skb(skb);
	}
}

__be16 eth_type_trans(struct sk_buff* skb, struct net_device* dev)
{
	struct ethhdr* eth;
	skb->dev = dev;
	skb_reset_mac_header(skb);
	eth = (struct ethhdr*)skb->data;
	skb_pull(skb, ETH_HLEN);
	if (is_multicast_ether_addr(eth->h_dest))
		skb->pkt_type = is_broadcast_ether_addr(eth->h_dest) ? PACKET_BROADCAST : PACKET_MULTICAST;
	else if (!ether_addr_equal(eth->h_dest, dev->dev_addr))
		skb->pkt_type = PACKET_OTHERHOST;
	if (ntohs(eth->h_proto) >= ETH_P_802_3_MIN)
		return eth->h_proto;
	return htons(ETH_P_802_2);
}

/* ---- NAPI ---- */

#define NAPI_SCHED 1UL
#define NAPI_DISABLED 2UL

static void napi_work(struct work_struct* w)
{
	struct napi_struct* napi = container_of(w, struct napi_struct, lkpi_work);
	int rounds;
	for (rounds = 0; rounds < 1000; rounds++) {
		if (napi->state & NAPI_DISABLED)
			break;
		napi->poll(napi, napi->weight);
		if (!(napi->state & NAPI_SCHED))
			break;
	}
}

void netif_napi_add(struct net_device* dev, struct napi_struct* napi, int (*poll)(struct napi_struct*, int))
{
	napi->dev = dev;
	napi->poll = poll;
	napi->weight = NAPI_POLL_WEIGHT;
	napi->state = NAPI_DISABLED;
	INIT_WORK(&napi->lkpi_work, napi_work);
}

void netif_napi_del(struct napi_struct* napi) { cancel_work_sync(&napi->lkpi_work); }
void napi_enable(struct napi_struct* napi) { __atomic_and_fetch(&napi->state, ~NAPI_DISABLED, __ATOMIC_SEQ_CST); }

void napi_disable(struct napi_struct* napi)
{
	__atomic_or_fetch(&napi->state, NAPI_DISABLED, __ATOMIC_SEQ_CST);
	cancel_work_sync(&napi->lkpi_work);
	__atomic_and_fetch(&napi->state, ~NAPI_SCHED, __ATOMIC_SEQ_CST);
}

bool napi_schedule_prep(struct napi_struct* napi)
{
	if (napi->state & NAPI_DISABLED)
		return false;
	return !(__atomic_fetch_or(&napi->state, NAPI_SCHED, __ATOMIC_SEQ_CST) & NAPI_SCHED);
}

void __napi_schedule(struct napi_struct* napi) { queue_work(system_highpri_wq, &napi->lkpi_work); }

bool napi_is_scheduled(struct napi_struct* napi) { return __atomic_load_n(&napi->state, __ATOMIC_RELAXED) & NAPI_SCHED; }

bool napi_complete_done(struct napi_struct* napi, int work_done)
{
	__atomic_and_fetch(&napi->state, ~NAPI_SCHED, __ATOMIC_SEQ_CST);
	return true;
}

/* ---- printing ---- */

int lkpi_netdev_printk(const char* level, const struct net_device* dev, const char* fmt, ...)
{
	char msg[400], line[440];
	va_list a;
	va_start(a, fmt);
	vsnprintf(msg, sizeof(msg), fmt, a);
	va_end(a);
	snprintf(line, sizeof(line), "%s: %s", dev ? dev->name : "(net)", msg);
	return lkpi_log_line(level, line);
}

/* ---- Ethernet helpers ---- */

int eth_validate_addr(struct net_device* dev)
{
	return is_valid_ether_addr(dev->dev_addr) ? 0 : -EADDRNOTAVAIL;
}

int eth_mac_addr(struct net_device* dev, void* p)
{
	struct sockaddr* sa = p;
	if (!is_valid_ether_addr((u8*)sa->sa_data))
		return -EADDRNOTAVAIL;
	eth_hw_addr_set(dev, (u8*)sa->sa_data);
	return 0;
}

void eth_random_addr(u8* addr)
{
	get_random_bytes(addr, ETH_ALEN);
	addr[0] &= 0xfe; /* unicast */
	addr[0] |= 0x02; /* locally administered */
}

u32 ethtool_op_get_link(struct net_device* dev)
{
	return netif_carrier_ok(dev) ? 1 : 0;
}

/* Software timestamps only (no PTP clocks here). */
int ethtool_op_get_ts_info(struct net_device* dev, struct kernel_ethtool_ts_info* eti)
{
	eti->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
	eti->phc_index = -1;
	return 0;
}

/* phylib hands the ethtool netlink code its operations: no ethtool netlink here. */
void ethtool_set_ethtool_phy_ops(const struct ethtool_phy_ops* ops)
{
}

u32 crc32_le(u32 crc, const unsigned char* p, size_t len)
{
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return crc;
}

u32 crc32_be(u32 crc, const unsigned char* p, size_t len)
{
	while (len--) {
		crc ^= (u32)*p++ << 24;
		for (int k = 0; k < 8; k++)
			crc = (crc << 1) ^ (0x04C11DB7u & (0u - (crc >> 31)));
	}
	return crc;
}

/* ---- MII (generic PHY helpers) ---- */

int mii_link_ok(struct mii_if_info* mii)
{
	mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR); /* latched: read twice */
	return (mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR) & BMSR_LSTATUS) ? 1 : 0;
}

int mii_nway_restart(struct mii_if_info* mii)
{
	int bmcr = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
	if (!(bmcr & BMCR_ANENABLE))
		return -EINVAL;
	mii->mdio_write(mii->dev, mii->phy_id, MII_BMCR, bmcr | BMCR_ANRESTART);
	return 0;
}

void mii_check_link(struct mii_if_info* mii)
{
	if (mii_link_ok(mii))
		netif_carrier_on(mii->dev);
	else
		netif_carrier_off(mii->dev);
}

unsigned int mii_check_media(struct mii_if_info* mii, unsigned int ok_to_print, unsigned int init_media)
{
	unsigned int old_carrier = netif_carrier_ok(mii->dev);
	unsigned int new_carrier = mii_link_ok(mii);
	int advertise, lpa;
	unsigned int media, duplex;
	bool changed;

	if (!init_media && old_carrier == new_carrier)
		return 0;
	if (!new_carrier) {
		netif_carrier_off(mii->dev);
		if (ok_to_print)
			netdev_info(mii->dev, "link down\n");
		return 0;
	}
	netif_carrier_on(mii->dev);
	if (mii->force_media)
		return 0;
	advertise = mii->advertising ? mii->advertising : mii->mdio_read(mii->dev, mii->phy_id, MII_ADVERTISE);
	lpa = mii->mdio_read(mii->dev, mii->phy_id, MII_LPA);
	media = (unsigned int)(advertise & lpa);
	duplex = (media & ADVERTISE_FULL) ? 1 : 0;
	if (ok_to_print)
		netdev_info(mii->dev, "link up, %uMbps, %s-duplex\n",
			    (media & (ADVERTISE_100FULL | ADVERTISE_100HALF)) ? 100 : 10, duplex ? "full" : "half");
	changed = mii->full_duplex != duplex || init_media;
	mii->full_duplex = duplex;
	return changed ? 1 : 0;
}

void mii_ethtool_get_link_ksettings(struct mii_if_info* mii, struct ethtool_link_ksettings* cmd)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->base.speed = SPEED_100;
	cmd->base.duplex = mii->full_duplex ? DUPLEX_FULL : DUPLEX_HALF;
	cmd->base.port = PORT_MII;
	cmd->base.autoneg = AUTONEG_ENABLE;
}

int mii_ethtool_set_link_ksettings(struct mii_if_info* mii, const struct ethtool_link_ksettings* cmd)
{
	return -EOPNOTSUPP;
}

static void lkpi_dummy_setup(struct net_device* dev) { }

struct net_device* alloc_netdev_dummy(int sizeof_priv)
{
	return alloc_netdev(sizeof_priv, "dummy#", NET_NAME_UNKNOWN, lkpi_dummy_setup);
}

/* The old ethtool interface (usbnet drivers): speed from what both ends offer. */
void mii_ethtool_gset(struct mii_if_info* mii, struct ethtool_cmd* ecmd)
{
	int bmcr = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
	int common = mii->mdio_read(mii->dev, mii->phy_id, MII_ADVERTISE) & mii->mdio_read(mii->dev, mii->phy_id, MII_LPA);
	int speed = (bmcr & BMCR_SPEED100) || (common & (ADVERTISE_100FULL | ADVERTISE_100HALF)) ? SPEED_100 : SPEED_10;
	if (mii->supports_gmii) {
		int stat1000 = mii->mdio_read(mii->dev, mii->phy_id, MII_STAT1000);
		if (stat1000 & (LPA_1000FULL | LPA_1000HALF))
			speed = SPEED_1000;
	}
	memset(ecmd, 0, sizeof(*ecmd));
	ecmd->speed = (u16)speed;
	ecmd->speed_hi = (u16)(speed >> 16);
	ecmd->duplex = mii->full_duplex ? DUPLEX_FULL : DUPLEX_HALF;
	ecmd->port = PORT_MII;
	ecmd->phy_address = (u8)mii->phy_id;
	ecmd->autoneg = (bmcr & BMCR_ANENABLE) ? AUTONEG_ENABLE : AUTONEG_DISABLE;
}

int mii_ethtool_sset(struct mii_if_info* mii, struct ethtool_cmd* ecmd)
{
	return -EOPNOTSUPP;
}

int generic_mii_ioctl(struct mii_if_info* mii, struct mii_ioctl_data* data, int cmd, unsigned int* duplex_changed)
{
	if (duplex_changed)
		*duplex_changed = 0;
	switch (cmd) {
	case 0x8947: /* SIOCGMIIPHY */
		data->phy_id = (u16)mii->phy_id;
		fallthrough;
	case 0x8948: /* SIOCGMIIREG */
		data->val_out = (u16)mii->mdio_read(mii->dev, data->phy_id, data->reg_num);
		return 0;
	case 0x8949: /* SIOCSMIIREG */
		mii->mdio_write(mii->dev, data->phy_id, data->reg_num, data->val_in);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* Before any driver's module_init (core_initcall). */
static int __init netdev_glue_init(void)
{
	refcount_set(&init_net.ns.count, 1);
	INIT_LIST_HEAD(&init_net.list);
	rx_init();
	return 0;
}
core_initcall(netdev_glue_init);

/* Called by the Syluxium stack (ethtool ioctl, /sys/class/net/<if>/speed):
 * the driver's link speed in Mbit/s and duplex. */
int lkpi_netdev_link_settings(struct net_device* dev, u32* speed, int* full_duplex)
{
	struct ethtool_link_ksettings ks;
	int ret;
	if (!dev->ethtool_ops || !dev->ethtool_ops->get_link_ksettings)
		return -EOPNOTSUPP;
	memset(&ks, 0, sizeof(ks));
	rtnl_lock();
	ret = dev->ethtool_ops->get_link_ksettings(dev, &ks);
	rtnl_unlock();
	if (ret)
		return ret;
	if (ks.base.speed == 0 || ks.base.speed == (u32)SPEED_UNKNOWN)
		return -ENODATA;
	*speed = ks.base.speed;
	*full_duplex = ks.base.duplex == DUPLEX_FULL;
	return 0;
}
