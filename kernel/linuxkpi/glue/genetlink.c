// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: generic netlink (the transport of nl80211).
 *
 * Syluxium's AF_NETLINK sockets (kernel/net/netlink.cpp) hand every message
 * sent on a NETLINK_GENERIC socket to lkpi_genl_rcv(). This file plays the
 * part of Linux's genetlink core: the "nlctrl" family (family/group lookup
 * by name, as libnl's genl_ctrl_resolve() does), attribute parsing with the
 * family's policy, pre_doit/doit/post_doit, dumps, and acks. Messages the
 * stack sends (replies, unicast events, multicast events) go back through
 * lkpi_sylux_netlink_deliver()/_multicast(); when the sender runs in atomic
 * context (RCU section, spinlock) they are queued for the "lkpi-netlink"
 * thread instead, because socket queues take sleeping locks.
 */
#include <linux/lkpi_all.h>
#include <linux/kthread.h>
#include <net/genetlink.h>
#include <net/sock.h>

#define NETLINK_GENERIC 16

/* ---- Syluxium side (kernel/net/netlink.cpp) ---- */
int lkpi_sylux_netlink_deliver(int protocol, u32 portid, const void* data, size_t len);
void lkpi_sylux_netlink_multicast(int protocol, u32 group, u32 exclude_portid, const void* data, size_t len);

static struct sock genl_sock = {.sk_protocol = NETLINK_GENERIC};

/* ---- delivery ---- */

struct pending_msg {
	struct list_head list;
	bool multicast;
	u32 portid; /* unicast: destination; multicast: excluded sender */
	u32 group;
	struct sk_buff* skb;
};
static LIST_HEAD(pending);
static DEFINE_SPINLOCK(pending_lock);
static DECLARE_WAIT_QUEUE_HEAD(pending_wait);
static bool delivery_started;

static bool can_sleep(void)
{
	return delivery_started && lkpi_preempt_count() == 0;
}

static void deliver_now(bool multicast, u32 portid, u32 group, struct sk_buff* skb)
{
	if (multicast)
		lkpi_sylux_netlink_multicast(NETLINK_GENERIC, group, portid, skb->data, skb->len);
	else
		lkpi_sylux_netlink_deliver(NETLINK_GENERIC, portid, skb->data, skb->len);
	kfree_skb(skb);
}

static void deliver(bool multicast, u32 portid, u32 group, struct sk_buff* skb)
{
	struct pending_msg* m;
	unsigned long flags;

	if (can_sleep()) {
		deliver_now(multicast, portid, group, skb);
		return;
	}
	m = kmalloc(sizeof(*m), GFP_ATOMIC);
	if (!m) {
		kfree_skb(skb);
		return;
	}
	m->multicast = multicast;
	m->portid = portid;
	m->group = group;
	m->skb = skb;
	spin_lock_irqsave(&pending_lock, flags);
	list_add_tail(&m->list, &pending);
	spin_unlock_irqrestore(&pending_lock, flags);
	wake_up(&pending_wait);
}

static int delivery_thread(void* arg)
{
	for (;;) {
		struct pending_msg* m;
		unsigned long flags;
		wait_event(pending_wait, !list_empty(&pending));
		for (;;) {
			spin_lock_irqsave(&pending_lock, flags);
			m = list_first_entry_or_null(&pending, struct pending_msg, list);
			if (m)
				list_del(&m->list);
			spin_unlock_irqrestore(&pending_lock, flags);
			if (!m)
				break;
			deliver_now(m->multicast, m->portid, m->group, m->skb);
			kfree(m);
		}
	}
	return 0;
}

int netlink_unicast(struct sock* ssk, struct sk_buff* skb, u32 portid, int nonblock)
{
	deliver(false, portid, 0, skb);
	return 0;
}

int netlink_broadcast_filtered(struct sock* ssk, struct sk_buff* skb, u32 portid, u32 group, gfp_t allocation,
			       netlink_filter_fn filter, void* filter_data)
{
	deliver(true, portid, group, skb);
	return 0;
}

int netlink_broadcast(struct sock* ssk, struct sk_buff* skb, u32 portid, u32 group, gfp_t allocation)
{
	return netlink_broadcast_filtered(ssk, skb, portid, group, allocation, NULL, NULL);
}

int netlink_has_listeners(struct sock* sk, unsigned int group)
{
	return 1;
}

struct nlmsghdr* __nlmsg_put(struct sk_buff* skb, u32 portid, u32 seq, int type, int len, int flags)
{
	struct nlmsghdr* nlh;
	int size = nlmsg_msg_size(len);

	nlh = skb_put(skb, NLMSG_ALIGN(size));
	nlh->nlmsg_type = type;
	nlh->nlmsg_len = size;
	nlh->nlmsg_flags = flags;
	nlh->nlmsg_pid = portid;
	nlh->nlmsg_seq = seq;
	if (NLMSG_ALIGN(size) - size != 0)
		memset(nlmsg_data(nlh) + len, 0, NLMSG_ALIGN(size) - size);
	return nlh;
}

/* ---- socket release notifications (NETLINK_URELEASE) ---- */

static RAW_NOTIFIER_HEAD(netlink_chain);
static DEFINE_MUTEX(netlink_chain_lock);

int netlink_register_notifier(struct notifier_block* nb)
{
	int r;
	mutex_lock(&netlink_chain_lock);
	r = raw_notifier_chain_register(&netlink_chain, nb);
	mutex_unlock(&netlink_chain_lock);
	return r;
}

int netlink_unregister_notifier(struct notifier_block* nb)
{
	int r;
	mutex_lock(&netlink_chain_lock);
	r = raw_notifier_chain_unregister(&netlink_chain, nb);
	mutex_unlock(&netlink_chain_lock);
	return r;
}

/* Called by Syluxium when a bound netlink socket is closed. */
void lkpi_netlink_release(int protocol, u32 portid)
{
	struct netlink_notify n = {.net = &init_net, .portid = portid, .protocol = protocol};
	mutex_lock(&netlink_chain_lock);
	raw_notifier_call_chain(&netlink_chain, NETLINK_URELEASE, &n);
	mutex_unlock(&netlink_chain_lock);
}

/* ---- families ---- */

#define MAX_FAMILIES 16
static const struct genl_family* families[MAX_FAMILIES];
static DEFINE_MUTEX(genl_mutex);   /* registration + non-parallel families */
static int next_family_id = GENL_ID_CTRL + 1;
static unsigned int next_mcgrp = 2;

void genl_lock(void) { mutex_lock(&genl_mutex); }
void genl_unlock(void) { mutex_unlock(&genl_mutex); }

static const struct genl_family* family_by_id(int id)
{
	for (int i = 0; i < MAX_FAMILIES; i++)
		if (families[i] && families[i]->id == id)
			return families[i];
	return NULL;
}

static const struct genl_family* family_by_name(const char* name)
{
	for (int i = 0; i < MAX_FAMILIES; i++)
		if (families[i] && !strcmp(families[i]->name, name))
			return families[i];
	return NULL;
}

int genl_register_family(struct genl_family* family)
{
	int slot = -1;
	mutex_lock(&genl_mutex);
	if (family_by_name(family->name)) {
		mutex_unlock(&genl_mutex);
		return -EEXIST;
	}
	for (int i = 0; i < MAX_FAMILIES; i++)
		if (!families[i]) {
			slot = i;
			break;
		}
	if (slot < 0) {
		mutex_unlock(&genl_mutex);
		return -ENOSPC;
	}
	family->id = next_family_id++;
	family->mcgrp_offset = next_mcgrp;
	next_mcgrp += family->n_mcgrps;
	if (next_mcgrp > GENL_ID_CTRL && family->mcgrp_offset <= GENL_ID_CTRL) {
		/* group 0x10 is nlctrl's "notify" */
		family->mcgrp_offset = GENL_ID_CTRL + 1;
		next_mcgrp = family->mcgrp_offset + family->n_mcgrps;
	}
	families[slot] = family;
	mutex_unlock(&genl_mutex);
	return 0;
}

int genl_unregister_family(const struct genl_family* family)
{
	mutex_lock(&genl_mutex);
	for (int i = 0; i < MAX_FAMILIES; i++)
		if (families[i] == family)
			families[i] = NULL;
	mutex_unlock(&genl_mutex);
	return 0;
}

void* genlmsg_put(struct sk_buff* skb, u32 portid, u32 seq, const struct genl_family* family, int flags, u8 cmd)
{
	struct nlmsghdr* nlh;
	struct genlmsghdr* hdr;

	nlh = nlmsg_put(skb, portid, seq, family->id, GENL_HDRLEN + family->hdrsize, flags);
	if (!nlh)
		return NULL;
	hdr = nlmsg_data(nlh);
	hdr->cmd = cmd;
	hdr->version = family->version;
	hdr->reserved = 0;
	return (char*)hdr + GENL_HDRLEN;
}

void genl_notify(const struct genl_family* family, struct sk_buff* skb, struct genl_info* info, u32 group, gfp_t flags)
{
	if (WARN_ON_ONCE(group >= family->n_mcgrps)) {
		kfree_skb(skb);
		return;
	}
	deliver(true, 0, family->mcgrp_offset + group, skb);
}

int genlmsg_multicast_allns(const struct genl_family* family, struct sk_buff* skb, u32 portid, unsigned int group)
{
	if (WARN_ON_ONCE(group >= family->n_mcgrps)) {
		kfree_skb(skb);
		return -EINVAL;
	}
	deliver(true, portid, family->mcgrp_offset + group, skb);
	return 0;
}

/* Find the operation for a command (split ops, full ops, then small ops). */
static bool get_op(const struct genl_family* family, u8 cmd, bool dump, struct genl_split_ops* op)
{
	int i;
	memset(op, 0, sizeof(*op));
	for (i = 0; i < family->n_split_ops; i++) {
		const struct genl_split_ops* s = &family->split_ops[i];
		if (s->cmd == cmd && !!(s->flags & GENL_CMD_CAP_DUMP) == dump) {
			*op = *s;
			return true;
		}
	}
	for (i = 0; i < family->n_ops; i++) {
		const struct genl_ops* o = &family->ops[i];
		if (o->cmd != cmd)
			continue;
		op->cmd = cmd;
		op->internal_flags = o->internal_flags;
		op->flags = o->flags;
		op->validate = o->validate;
		op->policy = o->policy ? o->policy : family->policy;
		op->maxattr = o->policy ? o->maxattr : family->maxattr;
		if (dump) {
			op->start = o->start;
			op->dumpit = o->dumpit;
			op->done = o->done;
			return o->dumpit != NULL;
		}
		op->pre_doit = family->pre_doit;
		op->doit = o->doit;
		op->post_doit = family->post_doit;
		return o->doit != NULL;
	}
	for (i = 0; i < family->n_small_ops; i++) {
		const struct genl_small_ops* o = &family->small_ops[i];
		if (o->cmd != cmd)
			continue;
		op->cmd = cmd;
		op->internal_flags = o->internal_flags;
		op->flags = o->flags;
		op->validate = o->validate;
		op->policy = family->policy;
		op->maxattr = family->maxattr;
		if (dump) {
			op->dumpit = o->dumpit;
			return o->dumpit != NULL;
		}
		op->pre_doit = family->pre_doit;
		op->doit = o->doit;
		op->post_doit = family->post_doit;
		return o->doit != NULL;
	}
	return false;
}

/* ---- acks / errors ---- */

static void send_ack(u32 portid, const struct nlmsghdr* req, int err)
{
	struct sk_buff* skb = nlmsg_new(sizeof(struct nlmsgerr), GFP_KERNEL);
	struct nlmsghdr* nlh;
	struct nlmsgerr* e;
	if (!skb)
		return;
	nlh = __nlmsg_put(skb, portid, req->nlmsg_seq, NLMSG_ERROR, sizeof(*e), NLM_F_CAPPED);
	e = nlmsg_data(nlh);
	e->error = err;
	e->msg = *req;
	deliver(false, portid, 0, skb);
}

static void send_done(u32 portid, const struct nlmsghdr* req, int err)
{
	struct sk_buff* skb = nlmsg_new(sizeof(int), GFP_KERNEL);
	struct nlmsghdr* nlh;
	if (!skb)
		return;
	nlh = __nlmsg_put(skb, portid, req->nlmsg_seq, NLMSG_DONE, sizeof(int), NLM_F_MULTI);
	memcpy(nlmsg_data(nlh), &err, sizeof(err));
	deliver(false, portid, 0, skb);
}

/* ---- nlctrl ---- */

static int ctrl_fill(struct sk_buff* skb, const struct genl_family* f, u32 portid, u32 seq, int flags)
{
	void* hdr = genlmsg_put(skb, portid, seq, &(struct genl_family){.id = GENL_ID_CTRL, .version = 2}, flags,
				CTRL_CMD_NEWFAMILY);
	struct nlattr *nest, *item;
	int i, idx = 1;
	if (!hdr)
		return -EMSGSIZE;
	if (nla_put_string(skb, CTRL_ATTR_FAMILY_NAME, f->name) || nla_put_u16(skb, CTRL_ATTR_FAMILY_ID, f->id) ||
	    nla_put_u32(skb, CTRL_ATTR_VERSION, f->version) || nla_put_u32(skb, CTRL_ATTR_HDRSIZE, f->hdrsize) ||
	    nla_put_u32(skb, CTRL_ATTR_MAXATTR, f->maxattr))
		goto fail;
	nest = nla_nest_start(skb, CTRL_ATTR_OPS);
	if (!nest)
		goto fail;
	for (u8 cmd = 0; cmd < 255; cmd++) {
		struct genl_split_ops op;
		u32 caps = 0;
		if (get_op(f, cmd, false, &op))
			caps |= GENL_CMD_CAP_DO | (op.flags & GENL_ADMIN_PERM);
		if (get_op(f, cmd, true, &op))
			caps |= GENL_CMD_CAP_DUMP | (op.flags & GENL_ADMIN_PERM);
		if (!caps)
			continue;
		if (op.policy)
			caps |= GENL_CMD_CAP_HASPOL;
		item = nla_nest_start(skb, idx++);
		if (!item || nla_put_u32(skb, CTRL_ATTR_OP_ID, cmd) || nla_put_u32(skb, CTRL_ATTR_OP_FLAGS, caps))
			goto fail;
		nla_nest_end(skb, item);
	}
	nla_nest_end(skb, nest);
	if (f->n_mcgrps) {
		nest = nla_nest_start(skb, CTRL_ATTR_MCAST_GROUPS);
		if (!nest)
			goto fail;
		for (i = 0; i < f->n_mcgrps; i++) {
			item = nla_nest_start(skb, i + 1);
			if (!item || nla_put_u32(skb, CTRL_ATTR_MCAST_GRP_ID, f->mcgrp_offset + i) ||
			    nla_put_string(skb, CTRL_ATTR_MCAST_GRP_NAME, f->mcgrps[i].name))
				goto fail;
			nla_nest_end(skb, item);
		}
		nla_nest_end(skb, nest);
	}
	genlmsg_end(skb, hdr);
	return 0;
fail:
	genlmsg_cancel(skb, hdr);
	return -EMSGSIZE;
}

static const struct nla_policy ctrl_policy[CTRL_ATTR_MAX + 1] = {
	[CTRL_ATTR_FAMILY_ID] = {.type = NLA_U16},
	[CTRL_ATTR_FAMILY_NAME] = {.type = NLA_NUL_STRING, .len = GENL_NAMSIZ - 1},
};

static const struct genl_family nlctrl_family = {
	.id = GENL_ID_CTRL, .name = "nlctrl", .version = 2, .maxattr = CTRL_ATTR_MAX, .n_mcgrps = 1,
	.mcgrps = (const struct genl_multicast_group[]){{.name = "notify"}}, .mcgrp_offset = GENL_ID_CTRL,
};

static void ctrl_rcv(u32 portid, const struct nlmsghdr* nlh)
{
	struct genlmsghdr* gh = nlmsg_data(nlh);
	struct nlattr* tb[CTRL_ATTR_MAX + 1];
	const struct genl_family* f = NULL;
	struct sk_buff* skb;
	int err;

	if (gh->cmd != CTRL_CMD_GETFAMILY) {
		send_ack(portid, nlh, -EOPNOTSUPP);
		return;
	}
	if ((nlh->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP) {
		/* All families, one message each. */
		skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
		if (skb && !ctrl_fill(skb, &nlctrl_family, portid, nlh->nlmsg_seq, NLM_F_MULTI))
			deliver(false, portid, 0, skb);
		else
			kfree_skb(skb);
		for (int i = 0; i < MAX_FAMILIES; i++) {
			if (!families[i])
				continue;
			skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
			if (skb && !ctrl_fill(skb, families[i], portid, nlh->nlmsg_seq, NLM_F_MULTI))
				deliver(false, portid, 0, skb);
			else
				kfree_skb(skb);
		}
		send_done(portid, nlh, 0);
		return;
	}
	err = nlmsg_parse_deprecated(nlh, GENL_HDRLEN, tb, CTRL_ATTR_MAX, ctrl_policy, NULL);
	if (err) {
		send_ack(portid, nlh, err);
		return;
	}
	if (tb[CTRL_ATTR_FAMILY_ID]) {
		u16 id = nla_get_u16(tb[CTRL_ATTR_FAMILY_ID]);
		f = id == GENL_ID_CTRL ? &nlctrl_family : family_by_id(id);
	} else if (tb[CTRL_ATTR_FAMILY_NAME]) {
		const char* name = nla_data(tb[CTRL_ATTR_FAMILY_NAME]);
		f = !strcmp(name, "nlctrl") ? &nlctrl_family : family_by_name(name);
	}
	if (!f) {
		send_ack(portid, nlh, -ENOENT);
		return;
	}
	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb) {
		send_ack(portid, nlh, -ENOMEM);
		return;
	}
	err = ctrl_fill(skb, f, portid, nlh->nlmsg_seq, 0);
	if (err) {
		kfree_skb(skb);
		send_ack(portid, nlh, err);
		return;
	}
	deliver(false, portid, 0, skb);
	if (nlh->nlmsg_flags & NLM_F_ACK)
		send_ack(portid, nlh, 0);
}

/* ---- requests ---- */

static int genl_dump(const struct genl_family* family, const struct genl_split_ops* op, struct sk_buff* req,
		     const struct nlmsghdr* nlh, u32 portid, struct nlattr** attrs, struct netlink_ext_ack* extack)
{
	struct genl_dumpit_info* dinfo = kzalloc(sizeof(*dinfo), GFP_KERNEL);
	struct netlink_callback* cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	int err = 0, rounds = 0;

	if (!dinfo || !cb) {
		kfree(dinfo);
		kfree(cb);
		return -ENOMEM;
	}
	dinfo->op = *op;
	dinfo->info.family = family;
	dinfo->info.snd_seq = nlh->nlmsg_seq;
	dinfo->info.snd_portid = portid;
	dinfo->info.nlhdr = nlh;
	dinfo->info.genlhdr = nlmsg_data(nlh);
	dinfo->info.attrs = attrs;
	dinfo->info.extack = extack;
	genl_info_net_set(&dinfo->info, &init_net);
	cb->skb = req;
	cb->nlh = nlh;
	cb->dump = op->dumpit;
	cb->done = op->done;
	cb->data = dinfo;
	cb->extack = extack;
	cb->family = NETLINK_GENERIC;
	cb->strict_check = true;

	if (op->start)
		err = op->start(cb);
	while (!err) {
		unsigned int size = max_t(unsigned int, NLMSG_GOODSIZE, cb->min_dump_alloc);
		struct sk_buff* skb = alloc_skb(size, GFP_KERNEL);
		int r;
		if (!skb) {
			err = -ENOMEM;
			break;
		}
		skb->sk = &genl_sock; /* dump handlers look up the namespace from it */
		r = op->dumpit(skb, cb);
		if (skb->len)
			deliver(false, portid, 0, skb);
		else
			kfree_skb(skb);
		if (r <= 0) {
			err = r;
			break;
		}
		if (++rounds > 10000) {
			err = -ELOOP;
			break;
		}
	}
	send_done(portid, nlh, err);
	if (op->done)
		op->done(cb);
	kfree(dinfo);
	kfree(cb);
	return 0;
}

static int genl_family_rcv(const struct genl_family* family, u32 portid, const struct nlmsghdr* nlh, struct sk_buff* req)
{
	struct genlmsghdr* gh = nlmsg_data(nlh);
	bool dump = (nlh->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP;
	struct netlink_ext_ack extack = {};
	struct genl_split_ops op;
	struct nlattr** attrs = NULL;
	struct genl_info info;
	int hdrlen = GENL_HDRLEN + family->hdrsize;
	int err;

	if (nlh->nlmsg_len < nlmsg_msg_size(hdrlen))
		return -EINVAL;
	if (!get_op(family, gh->cmd, dump, &op))
		return -EOPNOTSUPP;
	if (op.maxattr && op.policy) {
		attrs = kcalloc(op.maxattr + 1, sizeof(*attrs), GFP_KERNEL);
		if (!attrs)
			return -ENOMEM;
		if (!dump || !(op.validate & GENL_DONT_VALIDATE_DUMP)) {
			unsigned int validate = (op.validate & (dump ? GENL_DONT_VALIDATE_DUMP_STRICT : GENL_DONT_VALIDATE_STRICT))
							? NL_VALIDATE_LIBERAL
							: NL_VALIDATE_STRICT;
			err = __nlmsg_parse(nlh, hdrlen, attrs, op.maxattr, op.policy, validate, &extack);
			if (err) {
				if (extack._msg)
					printk(KERN_DEBUG "genetlink: %s: %s\n", family->name, extack._msg);
				kfree(attrs);
				return err;
			}
		}
	}
	if (!family->parallel_ops)
		genl_lock();
	if (dump) {
		err = genl_dump(family, &op, req, nlh, portid, attrs, &extack);
	} else {
		memset(&info, 0, sizeof(info));
		info.snd_seq = nlh->nlmsg_seq;
		info.snd_portid = portid;
		info.family = family;
		info.nlhdr = nlh;
		info.genlhdr = gh;
		info.attrs = attrs;
		info.extack = &extack;
		genl_info_net_set(&info, &init_net);
		err = 0;
		if (op.pre_doit)
			err = op.pre_doit(&op, req, &info);
		if (!err) {
			err = op.doit(req, &info);
			if (op.post_doit)
				op.post_doit(&op, req, &info);
		}
		if (err && extack._msg)
			printk(KERN_DEBUG "genetlink: %s cmd %u: %s (%d)\n", family->name, gh->cmd, extack._msg, err);
	}
	if (!family->parallel_ops)
		genl_unlock();
	kfree(attrs);
	return err;
}

/* Entry point: a buffer written to a NETLINK_GENERIC socket by `portid`. */
void lkpi_genl_rcv(u32 portid, const void* data, size_t len)
{
	const u8* p = data;
	while (len >= NLMSG_HDRLEN) {
		const struct nlmsghdr* in = (const void*)p;
		u32 msglen = in->nlmsg_len;
		struct sk_buff* req;
		struct nlmsghdr* nlh;
		int err;

		if (msglen < NLMSG_HDRLEN || msglen > len)
			break;
		/* Copy into an skb: handlers keep pointers into the message. */
		req = alloc_skb(msglen + 16, GFP_KERNEL);
		if (!req)
			break;
		nlh = skb_put_data(req, p, msglen);
		NETLINK_CB(req).portid = portid;
		NETLINK_CB(req).sk = &genl_sock;
		req->sk = &genl_sock;

		if (!(nlh->nlmsg_flags & NLM_F_REQUEST) || nlh->nlmsg_type < NLMSG_MIN_TYPE) {
			kfree_skb(req);
		} else if (msglen < NLMSG_HDRLEN + GENL_HDRLEN) {
			send_ack(portid, nlh, -EINVAL);
			kfree_skb(req);
		} else if (nlh->nlmsg_type == GENL_ID_CTRL) {
			ctrl_rcv(portid, nlh);
			kfree_skb(req);
		} else {
			const struct genl_family* family = family_by_id(nlh->nlmsg_type);
			bool dump = (nlh->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP;
			err = family ? genl_family_rcv(family, portid, nlh, req) : -ENOENT;
			/* Dumps end with NLMSG_DONE; others are acked on error or on request. */
			if (err || (!dump && (nlh->nlmsg_flags & NLM_F_ACK)))
				send_ack(portid, nlh, err);
			kfree_skb(req);
		}
		len -= min_t(size_t, NLMSG_ALIGN(msglen), len);
		p += NLMSG_ALIGN(msglen);
	}
}

static int __init genl_glue_init(void)
{
	init_net.genl_sock = &genl_sock;
	write_pnet(&genl_sock.sk_net, &init_net);
	delivery_started = true;
	kthread_run(delivery_thread, NULL, "lkpi-netlink");
	return 0;
}
core_initcall(genl_glue_init);
