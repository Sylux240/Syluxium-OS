/* LinuxKPI: tc flower hardware offload descriptions (Linux 7.4
 * include/net/flow_offload.h, include/net/flow_dissector.h). Syluxium has
 * no tc classifiers, so nothing ever hands a driver a flow rule; the types
 * and accessors let drivers that implement ndo_setup_tc (igb, ...) compile
 * unchanged, and the block setup answers -EOPNOTSUPP. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/netlink.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>

enum flow_dissector_key_id {
	FLOW_DISSECTOR_KEY_CONTROL,
	FLOW_DISSECTOR_KEY_BASIC,
	FLOW_DISSECTOR_KEY_IPV4_ADDRS,
	FLOW_DISSECTOR_KEY_IPV6_ADDRS,
	FLOW_DISSECTOR_KEY_PORTS,
	FLOW_DISSECTOR_KEY_PORTS_RANGE,
	FLOW_DISSECTOR_KEY_ICMP,
	FLOW_DISSECTOR_KEY_ETH_ADDRS,
	FLOW_DISSECTOR_KEY_TIPC,
	FLOW_DISSECTOR_KEY_ARP,
	FLOW_DISSECTOR_KEY_VLAN,
	FLOW_DISSECTOR_KEY_FLOW_LABEL,
	FLOW_DISSECTOR_KEY_GRE_KEYID,
	FLOW_DISSECTOR_KEY_MPLS_ENTROPY,
	FLOW_DISSECTOR_KEY_ENC_KEYID,
	FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS,
	FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS,
	FLOW_DISSECTOR_KEY_ENC_CONTROL,
	FLOW_DISSECTOR_KEY_ENC_PORTS,
	FLOW_DISSECTOR_KEY_MPLS,
	FLOW_DISSECTOR_KEY_TCP,
	FLOW_DISSECTOR_KEY_IP,
	FLOW_DISSECTOR_KEY_CVLAN,
	FLOW_DISSECTOR_KEY_ENC_IP,
	FLOW_DISSECTOR_KEY_ENC_OPTS,
	FLOW_DISSECTOR_KEY_META,
	FLOW_DISSECTOR_KEY_CT,
	FLOW_DISSECTOR_KEY_HASH,
	FLOW_DISSECTOR_KEY_NUM_OF_VLANS,
	FLOW_DISSECTOR_KEY_PPPOE,
	FLOW_DISSECTOR_KEY_L2TPV3,
	FLOW_DISSECTOR_KEY_CFM,
	FLOW_DISSECTOR_KEY_IPSEC,
	FLOW_DISSECTOR_KEY_MAX,
};

struct flow_dissector {
	unsigned long long used_keys;
	unsigned short offset[FLOW_DISSECTOR_KEY_MAX];
};

struct flow_dissector_key_control { u16 thoff; u16 addr_type; u32 flags; };
struct flow_dissector_key_basic { __be16 n_proto; u8 ip_proto; u8 padding; };
struct flow_dissector_key_eth_addrs { unsigned char dst[ETH_ALEN]; unsigned char src[ETH_ALEN]; };
struct flow_dissector_key_vlan {
	union {
		struct { u16 vlan_id:12, vlan_dei:1, vlan_priority:3; };
		__be16 vlan_tci;
	};
	__be16 vlan_tpid;
	__be16 vlan_eth_type;
	u16 padding;
};
struct flow_dissector_key_ipv4_addrs { __be32 src; __be32 dst; };
struct flow_dissector_key_ports { union { __be32 ports; struct { __be16 src; __be16 dst; }; }; };
struct flow_dissector_key_ip { __u8 tos; __u8 ttl; };

struct flow_match {
	struct flow_dissector *dissector;
	void *mask;
	void *key;
};
#define LKPI_FLOW_MATCH(name, type) \
	struct flow_match_##name { struct type *key, *mask; }
LKPI_FLOW_MATCH(control, flow_dissector_key_control);
LKPI_FLOW_MATCH(basic, flow_dissector_key_basic);
LKPI_FLOW_MATCH(eth_addrs, flow_dissector_key_eth_addrs);
LKPI_FLOW_MATCH(vlan, flow_dissector_key_vlan);
LKPI_FLOW_MATCH(ipv4_addrs, flow_dissector_key_ipv4_addrs);
LKPI_FLOW_MATCH(ports, flow_dissector_key_ports);
LKPI_FLOW_MATCH(ip, flow_dissector_key_ip);

struct flow_action { unsigned int num_entries; };
struct flow_rule {
	struct flow_match match;
	struct flow_action action;
};

enum flow_cls_command {
	FLOW_CLS_REPLACE, FLOW_CLS_DESTROY, FLOW_CLS_STATS, FLOW_CLS_TMPLT_CREATE, FLOW_CLS_TMPLT_DESTROY,
};
struct flow_cls_common_offload {
	u32 chain_index;
	__be16 protocol;
	u32 prio;
	bool skip_sw;
	struct netlink_ext_ack *extack;
};
struct flow_stats { u64 pkts; u64 bytes; u64 drops; u64 lastused; };
struct flow_cls_offload {
	struct flow_cls_common_offload common;
	enum flow_cls_command command;
	bool use_act_stats;
	unsigned long cookie;
	struct flow_rule *rule;
	struct flow_stats stats;
	u32 classid;
};
static inline struct flow_rule *flow_cls_offload_flow_rule(struct flow_cls_offload *f) { return f->rule; }

static inline bool flow_rule_match_key(const struct flow_rule *rule, enum flow_dissector_key_id key)
{
	return rule->match.dissector->used_keys & (1ull << key);
}
#define LKPI_FLOW_MATCH_FN(name, keyid)                                                              \
	static inline void flow_rule_match_##name(const struct flow_rule *rule, struct flow_match_##name *out) \
	{                                                                                            \
		const struct flow_match *m = &rule->match;                                               \
		out->key = (void *)((char *)m->key + m->dissector->offset[keyid]);                       \
		out->mask = (void *)((char *)m->mask + m->dissector->offset[keyid]);                     \
	}
LKPI_FLOW_MATCH_FN(control, FLOW_DISSECTOR_KEY_CONTROL)
LKPI_FLOW_MATCH_FN(basic, FLOW_DISSECTOR_KEY_BASIC)
LKPI_FLOW_MATCH_FN(eth_addrs, FLOW_DISSECTOR_KEY_ETH_ADDRS)
LKPI_FLOW_MATCH_FN(vlan, FLOW_DISSECTOR_KEY_VLAN)
LKPI_FLOW_MATCH_FN(ipv4_addrs, FLOW_DISSECTOR_KEY_IPV4_ADDRS)
LKPI_FLOW_MATCH_FN(ports, FLOW_DISSECTOR_KEY_PORTS)
LKPI_FLOW_MATCH_FN(ip, FLOW_DISSECTOR_KEY_IP)

static inline bool flow_rule_match_has_control_flags(struct flow_rule *rule, struct netlink_ext_ack *extack)
{
	struct flow_match_control m;
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL))
		return false;
	flow_rule_match_control(rule, &m);
	return m.mask->flags != 0;
}

/* Block binding: never requested (no tc), refused if it were. */
struct flow_block_offload;
typedef int flow_setup_cb_t(enum tc_setup_type type, void *type_data, void *cb_priv);
static inline int flow_block_cb_setup_simple(struct flow_block_offload *f, struct list_head *driver_list,
					     flow_setup_cb_t *cb, void *cb_ident, void *cb_priv, bool ingress_only)
{
	return -EOPNOTSUPP;
}
