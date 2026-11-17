#pragma once

#include <dev/device.hpp>
#include <lib/types.hpp>

// Networking: interfaces (network cards, loopback) and the TCP/IP stack.
// Programs use it through the Linux socket API (socket, connect, sendto,
// ...), so glibc's resolver, wget, ping and friends work unchanged.
namespace net {

constexpr u32 IFF_UP = 0x1, IFF_BROADCAST = 0x2, IFF_LOOPBACK = 0x8, IFF_RUNNING = 0x40, IFF_MULTICAST = 0x1000;

// A network interface. Drivers subclass it and implement transmit().
class Interface {
public:
    virtual ~Interface() = default;

    // Send one Ethernet frame (without FCS). Must not sleep for long; may
    // drop the frame when the hardware queue is full.
    virtual long transmit(const u8* frame, usize len) = 0;

    char name[16] = {};          // "eth0", "lo"
    u8 mac[6] = {};
    u32 mtu = 1500;
    u32 index = 0;               // ifindex, from 1
    u32 flags = IFF_UP | IFF_BROADCAST | IFF_MULTICAST | IFF_RUNNING;
    bool link_up = true;
    u32 addr = 0, netmask = 0;   // IPv4, host byte order (0 = unconfigured)
    dev::Device* hw = nullptr;

    u64 rx_packets = 0, rx_bytes = 0, rx_dropped = 0;
    u64 tx_packets = 0, tx_bytes = 0, tx_errors = 0;
};

// Called by drivers at probe time; names the interface eth0, eth1, ...
void register_interface(Interface* iface);

// Called by drivers for every received frame (from their RX thread).
void receive(Interface* iface, const u8* frame, usize len);

// Start the stack: loopback, timers, DHCP on every Ethernet interface.
// Waits up to `dhcp_wait_ms` for the first address so the shell starts online.
void init();
void start(u64 dhcp_wait_ms);

Interface* interface_by_index(u32 index);
Interface* interface_by_name(const char* name);
void for_each_interface(void (*fn)(Interface*, void*), void* ctx);

// Default route (host byte order, 0 = none) and DNS servers from DHCP.
u32 gateway();

// Format an IPv4 address (host order) as dotted decimal.
void format_ip(u32 ip, char* out, usize size);

} // namespace net

namespace virtio_net {
void register_driver();
}
namespace r8169 {
void register_driver(); // untested on hardware
}
namespace e1000 {
void register_driver();
}
