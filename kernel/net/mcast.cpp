// Multicast group membership (IPv4 and IPv6): which groups each interface
// receives, counted over the sockets that joined them. The network cards
// already pass every multicast frame up (all-multicast mode); the IP layers
// use mc_joined() to accept packets for joined groups, UDP to fan them out
// to the member sockets. Joining an IPv4 group sends an IGMPv2 membership
// report (and queries are answered), so switches that snoop IGMP forward
// the group to us. MLD for IPv6 is not done yet.

#include <net/internal.hpp>

namespace net {

namespace {

constexpr u8 PROTO_IGMP = 2;
constexpr u8 IGMP_QUERY = 0x11, IGMP_V2_REPORT = 0x16, IGMP_LEAVE = 0x17;
constexpr u32 ALL_ROUTERS4 = 0xE0000002;

struct Membership {
    bool used;
    bool v6;
    u32 ifindex;
    u32 addr4;
    u8 addr6[16];
    u32 refs;
};

constexpr usize MAX_MEMBERSHIPS = 64;
Membership g_mc[MAX_MEMBERSHIPS];

bool same_group(const Membership& m, const Endpoint& g) {
    return m.v6 == g.v6 && (g.v6 ? memcmp(m.addr6, g.addr6, 16) == 0 : m.addr4 == g.addr);
}

Membership* find(u32 ifindex, const Endpoint& g) {
    for (auto& m : g_mc)
        if (m.used && m.ifindex == ifindex && same_group(m, g))
            return &m;
    return nullptr;
}

void igmp_send(Interface* f, u8 type, u32 group) {
    if (!f || !f->addr || (f->flags & IFF_LOOPBACK))
        return;
    u8 msg[8] = {type, 0, 0, 0};
    put32(msg + 4, group);
    put16(msg + 2, checksum_finish(checksum_add(0, msg, 8)));
    ip_send(f->addr, type == IGMP_LEAVE ? ALL_ROUTERS4 : group, PROTO_IGMP, msg, 8, 1, f);
}

} // namespace

Interface* mc_interface(bool v6, u32 ifindex, u32 addr4) {
    if (ifindex) {
        Interface* f = interface_by_index(ifindex);
        return f && !f->detached ? f : nullptr;
    }
    if (addr4)
        for (u32 i = 1; Interface* f = interface_by_index(i); i++)
            if (f->addr == addr4)
                return f;
    // The first Ethernet interface that is up (with an address for IPv4).
    for (u32 i = 1; Interface* f = interface_by_index(i); i++)
        if (!(f->flags & IFF_LOOPBACK) && !f->detached && (f->flags & IFF_UP) && (v6 || f->addr))
            return f;
    return nullptr;
}

long mc_join(u32 ifindex, const Endpoint& group) {
    if (!is_mcast(group))
        return -err::INVAL;
    Interface* f = mc_interface(group.v6, ifindex);
    if (!f)
        return -err::NODEV;
    if (Membership* m = find(f->index, group)) {
        m->refs++;
        return f->index;
    }
    for (auto& m : g_mc) {
        if (m.used)
            continue;
        m = Membership{true, group.v6, f->index, group.v6 ? 0 : group.addr, {}, 1};
        if (group.v6)
            memcpy(m.addr6, group.addr6, 16);
        else
            igmp_send(f, IGMP_V2_REPORT, group.addr);
        return f->index;
    }
    return -ENOBUFS;
}

void mc_leave(u32 ifindex, const Endpoint& group) {
    Membership* m = find(ifindex, group);
    if (!m || --m->refs)
        return;
    m->used = false;
    if (!group.v6)
        igmp_send(interface_by_index(ifindex), IGMP_LEAVE, group.addr);
}

bool mc_joined(u32 ifindex, const Endpoint& group) {
    if (group.v6) {
        // ff01::1 / ff02::1: all nodes
        const u8* a = group.addr6;
        bool all_nodes = a[0] == 0xFF && (a[1] & 0x0F) <= 2 && a[15] == 1;
        for (int i = 2; i < 15 && all_nodes; i++)
            all_nodes = a[i] == 0;
        if (all_nodes)
            return true;
    } else if (group.addr == ALL_HOSTS4) {
        return true;
    }
    return find(ifindex, group) != nullptr;
}

void igmp_input(Interface* in, const u8* data, usize len) {
    if (len < 8 || data[0] != IGMP_QUERY)
        return;
    u32 asked = get32(data + 4); // 0: general query
    for (auto& m : g_mc)
        if (m.used && !m.v6 && m.ifindex == in->index && (!asked || asked == m.addr4))
            igmp_send(in, IGMP_V2_REPORT, m.addr4);
}

} // namespace net
