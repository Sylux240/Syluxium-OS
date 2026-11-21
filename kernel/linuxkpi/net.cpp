// LinuxKPI networking: the Syluxium side of struct net_device (glue/netdev.c).
// A registered Linux net_device becomes a net::Interface (eth0, wlan0, ...):
// frames the stack sends go to lkpi_netdev_xmit() (-> ndo_start_xmit),
// "ip link set X up/down" goes to lkpi_netdev_set_up() (-> ndo_open/ndo_stop),
// and frames the driver receives come back here through
// lkpi_sylux_netif_rx() (called from the "lkpi-netrx" thread).

#include <core/log.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <net/net.hpp>
#include <proc/uaccess.hpp>
#include <lib/errno.hpp>
#include <sched/sync.hpp>

extern "C" {
long lkpi_netdev_xmit(void* dev, const u8* frame, usize len);
long lkpi_netdev_set_up(void* dev, int up);
int lkpi_netdev_link_settings(void* dev, u32* speed, int* full_duplex);
}

namespace {

// The driver's net_device can disappear (USB unplug) while the stack still
// holds the interface: calls into the driver go through m_lock and stop
// once detach_driver() has run. A replugged device revives the interface
// of the same name (same wlan0, same ifindex for open sockets).
class LinuxNetIf : public net::Interface {
public:
    explicit LinuxNetIf(void* d) : m_dev(d) {}

    long transmit(const u8* frame, usize len) override {
        LockGuard guard(m_lock);
        return m_dev ? lkpi_netdev_xmit(m_dev, frame, len) : -err::NODEV;
    }

    long set_up(bool up) override {
        LockGuard guard(m_lock);
        return m_dev ? lkpi_netdev_set_up(m_dev, up ? 1 : 0) : -err::NODEV;
    }

    bool link_settings(u32* mbps, bool* full_duplex) override {
        LockGuard guard(m_lock);
        int full = 0;
        if (!m_dev || lkpi_netdev_link_settings(m_dev, mbps, &full) != 0)
            return false;
        *full_duplex = full != 0;
        return true;
    }

    void detach_driver() override {
        LockGuard guard(m_lock);
        m_dev = nullptr;
    }

    void reattach(void* d) {
        LockGuard guard(m_lock);
        m_dev = d;
    }

    LinuxNetIf* next_linux = nullptr;

private:
    sched::Mutex m_lock;
    void* m_dev; // struct net_device*
};

LinuxNetIf* g_linux_ifs = nullptr; // every one ever registered (never freed)

} // namespace

extern "C" {

void* lkpi_sylux_netif_register(void* dev, const char* name, const u8* mac, unsigned int mtu, void* hw, int wireless,
                                int up, int* ifindex) {
    // Replugged hardware: revive its old interface.
    for (LinuxNetIf* old = g_linux_ifs; old; old = old->next_linux) {
        if (old->detached && strcmp(old->name, name) == 0) {
            memcpy(old->mac, mac, 6);
            old->mtu = mtu;
            old->hw = static_cast<dev::Device*>(hw);
            old->flags = up ? (net::IFF_UP | net::IFF_BROADCAST | net::IFF_MULTICAST | net::IFF_RUNNING)
                            : (net::IFF_BROADCAST | net::IFF_MULTICAST);
            old->link_up = !wireless;
            old->reattach(dev);
            old->detached = false;
            klog::info("net: %s is back", old->name);
            *ifindex = static_cast<int>(old->index);
            return old;
        }
    }
    auto* iface = new LinuxNetIf(dev);
    iface->next_linux = g_linux_ifs;
    g_linux_ifs = iface;
    snprintf(iface->name, sizeof(iface->name), "%s", name);
    memcpy(iface->mac, mac, 6);
    iface->mtu = mtu;
    iface->hw = static_cast<dev::Device*>(hw);
    // Ethernet drivers are opened right after registration (up); Wi-Fi
    // interfaces start down, without link, until "ip link set wlan0 up" and
    // an association.
    iface->flags = up ? (net::IFF_UP | net::IFF_BROADCAST | net::IFF_MULTICAST | net::IFF_RUNNING)
                      : (net::IFF_BROADCAST | net::IFF_MULTICAST);
    iface->link_up = !wireless;
    net::register_interface(iface);
    *ifindex = static_cast<int>(iface->index);
    return iface;
}

void lkpi_sylux_netif_unregister(void* iface) { net::unregister_interface(static_cast<net::Interface*>(iface)); }

void lkpi_sylux_netif_link(void* iface, int carrier) {
    auto* f = static_cast<net::Interface*>(iface);
    if (carrier && !f->link_up) {
        klog::info("Network: %s link up", f->name);
        net::link_up(f);
    } else if (!carrier && f->link_up) {
        klog::info("Network: %s link down", f->name);
        f->link_up = false;
    }
}

void lkpi_sylux_netif_flags(void* iface, int up) {
    auto* f = static_cast<net::Interface*>(iface);
    f->flags = up ? (f->flags | net::IFF_UP) : (f->flags & ~net::IFF_UP);
}

void lkpi_sylux_netif_mac(void* iface, const u8* mac) { memcpy(static_cast<net::Interface*>(iface)->mac, mac, 6); }

void lkpi_sylux_netif_rx(void* iface, const u8* frame, usize len) {
    net::receive(static_cast<net::Interface*>(iface), frame, len);
}

int lkpi_sylux_name_taken(const char* name) {
    net::Interface* i = net::interface_by_name(name);
    return i && !i->detached; // a removed device's name is free for its replug
}

unsigned long copy_to_user(void* to, const void* from, unsigned long n) { return uaccess::copy_to(to, from, n) < 0 ? n : 0; }
unsigned long copy_from_user(void* to, const void* from, unsigned long n) {
    return uaccess::copy_from(to, from, n) < 0 ? n : 0;
}

} // extern "C"
