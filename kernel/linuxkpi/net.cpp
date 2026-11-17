// LinuxKPI networking: struct net_device / sk_buff / NAPI / MII on top of
// Syluxium's network stack. A Linux network driver's net_device becomes a
// Syluxium net::Interface (eth0, eth1, ...): frames the stack sends go to
// ndo_start_xmit(), frames the driver hands to netif_receive_skb() go to
// net::receive().

#include <core/log.hpp>
#include <lib/format.hpp>
#include <net/net.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

namespace {

long sylux_copy_to_user(void* to, const void* from, usize n) { return uaccess::copy_to(to, from, n); }
long sylux_copy_from_user(void* to, const void* from, usize n) { return uaccess::copy_from(to, from, n); }

} // namespace

#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/mii.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#undef min
#undef max
#undef abs
#undef swap
#undef clamp

extern "C" int lkpi_log_line(const char* level, const char* text);

namespace {

// The Syluxium side of a Linux net_device.
class LinuxNetIf : public net::Interface {
public:
    explicit LinuxNetIf(struct net_device* d) : m_dev(d) {}

    long transmit(const u8* frame, usize len) override {
        if (!m_dev->lkpi_running || m_dev->lkpi_queue_stopped)
            return -EAGAIN;
        struct sk_buff* skb = alloc_skb(static_cast<unsigned>(len) + 64, GFP_ATOMIC);
        if (!skb)
            return -ENOMEM;
        skb_reserve(skb, 32);
        memcpy(skb_put(skb, static_cast<unsigned>(len)), frame, len);
        skb->dev = m_dev;
        netdev_tx_t r = m_dev->netdev_ops->ndo_start_xmit(skb, m_dev);
        if (r != NETDEV_TX_OK) {
            kfree_skb(skb); // NETDEV_TX_BUSY: the driver didn't take it
            return -EAGAIN;
        }
        return 0;
    }

private:
    struct net_device* m_dev;
};

sched::Mutex g_rtnl;

void napi_work(struct work_struct* w);

constexpr unsigned long NAPI_SCHED = 1, NAPI_DISABLED = 2;

} // namespace

extern "C" {

// ---- sk_buff ----

struct skb_shared_info lkpi_empty_shinfo;

struct sk_buff* alloc_skb(unsigned int size, gfp_t gfp) {
    auto* skb = static_cast<struct sk_buff*>(kzalloc(sizeof(struct sk_buff), gfp));
    if (!skb)
        return nullptr;
    skb->head = static_cast<unsigned char*>(kmalloc(size + NET_SKB_PAD, gfp));
    if (!skb->head) {
        kfree(skb);
        return nullptr;
    }
    skb->data = skb->tail = skb->head;
    skb->end = skb->head + size + NET_SKB_PAD;
    return skb;
}

struct sk_buff* netdev_alloc_skb(struct net_device* dev, unsigned int len) {
    struct sk_buff* skb = alloc_skb(len + NET_SKB_PAD, GFP_ATOMIC);
    if (skb) {
        skb_reserve(skb, NET_SKB_PAD);
        skb->dev = dev;
    }
    return skb;
}

struct sk_buff* napi_alloc_skb(struct napi_struct* napi, unsigned int len) {
    return netdev_alloc_skb(napi ? napi->dev : nullptr, len);
}

void kfree_skb(struct sk_buff* skb) {
    if (!skb)
        return;
    kfree(skb->head);
    kfree(skb);
}

struct sk_buff* skb_copy(const struct sk_buff* skb, gfp_t gfp) {
    struct sk_buff* n = alloc_skb(static_cast<unsigned>(skb->end - skb->head), gfp);
    if (!n)
        return nullptr;
    skb_reserve(n, static_cast<int>(skb->data - skb->head));
    memcpy(skb_put(n, skb->len), skb->data, skb->len);
    n->dev = skb->dev;
    n->protocol = skb->protocol;
    return n;
}

// ---- net_device ----

struct net_device* alloc_etherdev_mqs(int sizeof_priv, unsigned int, unsigned int) {
    auto* dev = static_cast<struct net_device*>(kzalloc(sizeof(struct net_device) + sizeof_priv, GFP_KERNEL));
    if (!dev)
        return nullptr;
    snprintf(dev->name, sizeof(dev->name), "eth%%d");
    dev->mtu = ETH_DATA_LEN;
    dev->min_mtu = ETH_MIN_MTU;
    dev->max_mtu = ETH_DATA_LEN;
    dev->addr_len = ETH_ALEN;
    dev->hard_header_len = ETH_HLEN;
    dev->flags = IFF_BROADCAST | IFF_MULTICAST;
    dev->tx_queue_len = 1000;
    dev->watchdog_timeo = 5000;
    dev->dev_addr = dev->lkpi_addr;
    memset(dev->broadcast, 0xff, ETH_ALEN);
    dev->lkpi_priv_size = static_cast<size_t>(sizeof_priv);
    dev->lkpi_carrier = 1;
    return dev;
}

void free_netdev(struct net_device* dev) { kfree(dev); }

// Register = make it a Syluxium interface and bring it up (Linux would wait
// for "ip link set up"; Syluxium's interfaces are up from boot).
int register_netdev(struct net_device* dev) {
    auto* iface = new LinuxNetIf(dev);
    memcpy(iface->mac, dev->dev_addr, 6);
    iface->mtu = dev->mtu;
    if (dev->dev.parent)
        iface->hw = static_cast<dev::Device*>(dev->dev.parent->lkpi_registry);
    dev->lkpi_iface = iface;
    if (dev->netdev_ops->ndo_open) {
        rtnl_lock();
        dev->lkpi_running = 1;
        dev->flags |= IFF_UP;
        int r = dev->netdev_ops->ndo_open(dev);
        rtnl_unlock();
        if (r) {
            dev->lkpi_running = 0;
            dev->flags &= ~IFF_UP;
            delete iface;
            dev->lkpi_iface = nullptr;
            return r;
        }
    }
    iface->link_up = dev->lkpi_carrier;
    net::register_interface(iface);
    snprintf(dev->name, sizeof(dev->name), "%s", iface->name);
    return 0;
}

void unregister_netdev(struct net_device* dev) {
    if (dev->netdev_ops->ndo_stop && dev->lkpi_running) {
        rtnl_lock();
        dev->netdev_ops->ndo_stop(dev);
        rtnl_unlock();
    }
    dev->lkpi_running = 0;
    // Interfaces are never removed from the stack yet (no hot-unplug).
}

void netif_wake_queue(struct net_device* dev) { dev->lkpi_queue_stopped = 0; }

void netif_carrier_on(struct net_device* dev) {
    dev->lkpi_carrier = 1;
    if (dev->lkpi_iface)
        static_cast<LinuxNetIf*>(dev->lkpi_iface)->link_up = true;
}

void netif_carrier_off(struct net_device* dev) {
    dev->lkpi_carrier = 0;
    if (dev->lkpi_iface)
        static_cast<LinuxNetIf*>(dev->lkpi_iface)->link_up = false;
}

void netdev_update_features(struct net_device*) {}

u32 ethtool_op_get_link(struct net_device* dev) { return netif_carrier_ok(dev) ? 1 : 0; }

int lkpi_netdev_printk(const char* level, const struct net_device* dev, const char* fmt, ...) {
    char msg[400], line[440];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    snprintf(line, sizeof(line), "%s: %s", dev ? dev->name : "(net)", msg);
    return lkpi_log_line(level, line);
}

// ---- receive path ----

u16 eth_type_trans(struct sk_buff* skb, struct net_device* dev) {
    skb->dev = dev;
    skb_reset_mac_header(skb);
    auto* eth = reinterpret_cast<struct ethhdr*>(skb->data);
    skb_pull(skb, ETH_HLEN);
    return eth->h_proto;
}

int netif_receive_skb(struct sk_buff* skb) {
    struct net_device* dev = skb->dev;
    unsigned char* frame = skb->mac_header_ptr ? skb->mac_header_ptr : skb->data;
    usize len = static_cast<usize>(skb->tail - frame);
    if (dev && dev->lkpi_iface && len >= ETH_HLEN)
        net::receive(static_cast<LinuxNetIf*>(dev->lkpi_iface), frame, len);
    kfree_skb(skb);
    return 0;
}

// ---- NAPI ----

void netif_napi_add(struct net_device* dev, struct napi_struct* napi, int (*poll)(struct napi_struct*, int)) {
    napi->dev = dev;
    napi->poll = poll;
    napi->weight = NAPI_POLL_WEIGHT;
    napi->state = NAPI_DISABLED;
    INIT_WORK(&napi->lkpi_work, napi_work);
}

void netif_napi_del(struct napi_struct* napi) { cancel_work_sync(&napi->lkpi_work); }

void napi_enable(struct napi_struct* napi) { __atomic_and_fetch(&napi->state, ~NAPI_DISABLED, __ATOMIC_SEQ_CST); }

void napi_disable(struct napi_struct* napi) {
    __atomic_or_fetch(&napi->state, NAPI_DISABLED, __ATOMIC_SEQ_CST);
    cancel_work_sync(&napi->lkpi_work);
    __atomic_and_fetch(&napi->state, ~NAPI_SCHED, __ATOMIC_SEQ_CST);
}

bool napi_schedule_prep(struct napi_struct* napi) {
    if (napi->state & NAPI_DISABLED)
        return false;
    return !(__atomic_fetch_or(&napi->state, NAPI_SCHED, __ATOMIC_SEQ_CST) & NAPI_SCHED);
}

void __napi_schedule(struct napi_struct* napi) { queue_work(system_highpri_wq, &napi->lkpi_work); }

bool napi_complete_done(struct napi_struct* napi, int) {
    __atomic_and_fetch(&napi->state, ~NAPI_SCHED, __ATOMIC_SEQ_CST);
    return true;
}

void rtnl_lock(void) { g_rtnl.lock(); }
void rtnl_unlock(void) { g_rtnl.unlock(); }

// ---- Ethernet helpers ----

int eth_validate_addr(struct net_device* dev) { return is_valid_ether_addr(dev->dev_addr) ? 0 : -EADDRNOTAVAIL; }

int eth_mac_addr(struct net_device* dev, void* p) {
    auto* sa = static_cast<struct sockaddr*>(p);
    if (!is_valid_ether_addr(reinterpret_cast<u8*>(sa->sa_data)))
        return -EADDRNOTAVAIL;
    eth_hw_addr_set(dev, reinterpret_cast<u8*>(sa->sa_data));
    return 0;
}

void eth_random_addr(u8* addr) {
    u64 t = clock::now_ns() * 6364136223846793005ull + 1442695040888963407ull;
    for (int i = 0; i < 6; i++)
        addr[i] = static_cast<u8>(t >> (i * 8));
    addr[0] = static_cast<u8>((addr[0] & 0xFE) | 0x02); // locally administered unicast
}

u32 crc32_le(u32 crc, const unsigned char* p, size_t len) {
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}

u32 crc32_be(u32 crc, const unsigned char* p, size_t len) {
    while (len--) {
        crc ^= static_cast<u32>(*p++) << 24;
        for (int k = 0; k < 8; k++)
            crc = (crc << 1) ^ (0x04C11DB7u & (0u - (crc >> 31)));
    }
    return crc;
}

unsigned long copy_to_user(void __user* to, const void* from, unsigned long n) {
    return sylux_copy_to_user(to, from, n) < 0 ? n : 0;
}
unsigned long copy_from_user(void* to, const void __user* from, unsigned long n) {
    return sylux_copy_from_user(to, from, n) < 0 ? n : 0;
}

// ---- MII (generic PHY helpers) ----

int mii_link_ok(struct mii_if_info* mii) {
    mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR); // latched: read twice
    return (mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR) & BMSR_LSTATUS) ? 1 : 0;
}

int mii_nway_restart(struct mii_if_info* mii) {
    int bmcr = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
    if (!(bmcr & BMCR_ANENABLE))
        return -EINVAL;
    mii->mdio_write(mii->dev, mii->phy_id, MII_BMCR, bmcr | BMCR_ANRESTART);
    return 0;
}

void mii_check_link(struct mii_if_info* mii) {
    if (mii_link_ok(mii))
        netif_carrier_on(mii->dev);
    else
        netif_carrier_off(mii->dev);
}

unsigned int mii_check_media(struct mii_if_info* mii, unsigned int ok_to_print, unsigned int init_media) {
    unsigned int old_carrier = netif_carrier_ok(mii->dev);
    unsigned int new_carrier = mii_link_ok(mii);
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
    int advertise = mii->advertising ? mii->advertising : mii->mdio_read(mii->dev, mii->phy_id, MII_ADVERTISE);
    int lpa = mii->mdio_read(mii->dev, mii->phy_id, MII_LPA);
    unsigned int media = static_cast<unsigned>(advertise & lpa);
    unsigned int duplex = (media & ADVERTISE_FULL) ? 1 : 0;
    if (ok_to_print)
        netdev_info(mii->dev, "link up, %uMbps, %s-duplex\n", (media & (ADVERTISE_100FULL | ADVERTISE_100HALF)) ? 100 : 10,
                    duplex ? "full" : "half");
    bool changed = mii->full_duplex != duplex || init_media;
    mii->full_duplex = duplex;
    return changed ? 1 : 0;
}

void mii_ethtool_get_link_ksettings(struct mii_if_info* mii, struct ethtool_link_ksettings* cmd) {
    memset(cmd, 0, sizeof(*cmd));
    cmd->base.speed = SPEED_100;
    cmd->base.duplex = mii->full_duplex ? DUPLEX_FULL : DUPLEX_HALF;
    cmd->base.port = PORT_MII;
    cmd->base.autoneg = AUTONEG_ENABLE;
}

int mii_ethtool_set_link_ksettings(struct mii_if_info*, const struct ethtool_link_ksettings*) { return -EOPNOTSUPP; }

int generic_mii_ioctl(struct mii_if_info* mii, struct mii_ioctl_data* data, int cmd, unsigned int* duplex_changed) {
    if (duplex_changed)
        *duplex_changed = 0;
    switch (cmd) {
    case 0x8947: // SIOCGMIIPHY
        data->phy_id = static_cast<u16>(mii->phy_id);
        fallthrough;
    case 0x8948: // SIOCGMIIREG
        data->val_out = static_cast<u16>(mii->mdio_read(mii->dev, data->phy_id, data->reg_num));
        return 0;
    case 0x8949: // SIOCSMIIREG
        mii->mdio_write(mii->dev, data->phy_id, data->reg_num, data->val_in);
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

} // extern "C"

namespace {

// NAPI poll loop: keep polling while the driver uses its whole budget,
// stop once it calls napi_complete_done() (it re-enables its interrupt).
void napi_work(struct work_struct* w) {
    auto* napi = container_of(w, struct napi_struct, lkpi_work);
    for (int rounds = 0; rounds < 1000; rounds++) {
        if (napi->state & NAPI_DISABLED)
            break;
        int done = napi->poll(napi, napi->weight);
        if (!(napi->state & NAPI_SCHED))
            break;
        if (done < napi->weight) {
            // The driver didn't complete; poll again shortly (polling mode).
            continue;
        }
    }
}

} // namespace
