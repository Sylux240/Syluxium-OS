// USB Ethernet: CDC-ECM ("Ethernet Control Model"), the standard class for
// USB network adapters and phone/tablet tethering (and the second
// configuration of many vendor dongles, e.g. Realtek RTL8152/8153).
//
// A communication interface (class 2, subclass 6) carries the descriptors
// (union: which data interface; Ethernet: the MAC address string) and an
// interrupt endpoint with link notifications; the data interface's
// alternate setting 1 has the bulk endpoints that carry Ethernet frames.

#include <core/log.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <net/net.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>

namespace usb::cdc_ether {

namespace {

constexpr u8 CS_INTERFACE = 0x24, FUNC_UNION = 0x06, FUNC_ETHERNET = 0x0F;
constexpr u8 REQ_SET_INTERFACE = 11, REQ_SET_PACKET_FILTER = 0x43;
constexpr u8 NOTIFY_NETWORK_CONNECTION = 0x00;
constexpr usize FRAME_MAX = 1536, TX_SLOTS = 32;

class EcmInterface : public net::Interface {
public:
    Device* usb = nullptr;
    u8 bulk_in = 0, bulk_out = 0;
    u16 out_max_packet = 64;
    bool gone = false;

    long transmit(const u8* frame, usize len) override {
        if (gone || len > FRAME_MAX - 1)
            return -err::NODEV;
        {
            LockGuard guard(m_tx_lock);
            if (m_tx_count == TX_SLOTS) {
                tx_errors++;
                return 0; // queue full: drop, like a busy NIC
            }
            usize slot = (m_tx_head + m_tx_count) % TX_SLOTS;
            memcpy(m_tx_buf + slot * FRAME_MAX, frame, len);
            m_tx_len[slot] = static_cast<u32>(len);
            m_tx_count++;
        }
        m_tx_wait.wake_all();
        return 0;
    }

    void tx_loop() {
        u8* frame = static_cast<u8*>(kmalloc(FRAME_MAX));
        while (!gone) {
            m_tx_wait.wait_until([&] { return m_tx_count > 0 || gone; });
            if (gone)
                break;
            u32 len;
            {
                LockGuard guard(m_tx_lock);
                memcpy(frame, m_tx_buf + m_tx_head * FRAME_MAX, m_tx_len[m_tx_head]);
                len = m_tx_len[m_tx_head];
                m_tx_head = (m_tx_head + 1) % TX_SLOTS;
                m_tx_count--;
            }
            // A transfer that ends on a packet boundary needs a short packet
            // after it; padding by one byte does the same for Ethernet.
            if (len % out_max_packet == 0)
                frame[len++] = 0;
            long r = usb->transfer(bulk_out, frame, len);
            if (r < 0) {
                tx_errors++;
                if (r == -err::NODEV)
                    gone = true;
            } else {
                tx_packets++;
                tx_bytes += len;
            }
        }
        kfree(frame);
    }

    void rx_loop() {
        u8* frame = static_cast<u8*>(kmalloc(FRAME_MAX + 512));
        while (!gone) {
            long r = usb->transfer_wait(bulk_in, frame, FRAME_MAX + 512);
            if (r < 0) {
                if (r == -err::NODEV)
                    gone = true;
                else
                    sched::sleep_ms(10);
                continue;
            }
            if (r >= 14)
                net::receive(this, frame, static_cast<usize>(r));
        }
        kfree(frame);
        link_up = false;
        flags &= ~net::IFF_RUNNING;
        m_tx_wait.wake_all();
    }

    static void notification(void* ctx, const u8* data, u32 length) {
        auto* self = static_cast<EcmInterface*>(ctx);
        if (length >= 8 && data[0] == 0xA1 && data[1] == NOTIFY_NETWORK_CONNECTION) {
            bool up = data[2] != 0;
            if (up != self->link_up) {
                self->link_up = up;
                if (up)
                    self->flags |= net::IFF_RUNNING;
                else
                    self->flags &= ~net::IFF_RUNNING;
            }
        }
    }

    static void disconnect(void* ctx) {
        auto* self = static_cast<EcmInterface*>(ctx);
        self->gone = true;
        self->link_up = false;
        self->flags &= ~net::IFF_RUNNING;
        self->m_tx_wait.wake_all();
        klog::info("Network: %s unplugged", self->name);
    }

    u8* m_tx_buf = static_cast<u8*>(kmalloc(TX_SLOTS * FRAME_MAX));
    u32 m_tx_len[TX_SLOTS] = {};
    usize m_tx_head = 0, m_tx_count = 0;
    IrqSpinlock m_tx_lock;
    sched::WaitQueue m_tx_wait;
};

int hex(u16 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool read_mac(Device* d, u8 index, u8 mac[6]) {
    u8 buf[64];
    long n = d->control(0x80, REQ_GET_DESCRIPTOR, static_cast<u16>((DESC_STRING << 8) | index), 0x0409, buf, sizeof(buf));
    if (n < 2 + 24)
        return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex(static_cast<u16>(buf[2 + i * 4] | (buf[3 + i * 4] << 8)));
        int lo = hex(static_cast<u16>(buf[4 + i * 4] | (buf[5 + i * 4] << 8)));
        if (hi < 0 || lo < 0)
            return false;
        mac[i] = static_cast<u8>(hi << 4 | lo);
    }
    return true;
}

void tx_thread(void* arg) { static_cast<EcmInterface*>(arg)->tx_loop(); }
void rx_thread(void* arg) { static_cast<EcmInterface*>(arg)->rx_loop(); }

} // namespace

bool probe(Device* d, const Interface& intf) {
    if (intf.cls != 2 || intf.subclass != 6)
        return false;
    // The descriptors that follow our communication interface.
    const u8* c = d->config_desc;
    u16 total = d->config_len;
    int data_if = -1;
    u8 mac_index = 0;
    bool ours = false;
    for (u16 off = 0; off + 2 <= total && c[off] >= 2; off = static_cast<u16>(off + c[off])) {
        if (c[off + 1] == DESC_INTERFACE && off + 9 <= total) {
            ours = c[off + 2] == intf.number && c[off + 5] == 2;
            continue;
        }
        if (!ours || c[off + 1] != CS_INTERFACE || off + 3 > total)
            continue;
        if (c[off + 2] == FUNC_UNION && c[off] >= 5)
            data_if = c[off + 4];
        else if (c[off + 2] == FUNC_ETHERNET && c[off] >= 13)
            mac_index = c[off + 3];
    }
    if (data_if < 0)
        return false;
    // The data interface's alternate setting with the bulk endpoints.
    Endpoint in{}, out{};
    int alt = -1;
    int cur_if = -1, cur_alt = 0;
    for (u16 off = 0; off + 2 <= total && c[off] >= 2; off = static_cast<u16>(off + c[off])) {
        if (c[off + 1] == DESC_INTERFACE && off + 9 <= total) {
            cur_if = c[off + 2];
            cur_alt = c[off + 3];
        } else if (c[off + 1] == DESC_ENDPOINT && cur_if == data_if && off + 7 <= total && (c[off + 3] & 3) == 2) {
            Endpoint e{c[off + 2], c[off + 3], static_cast<u16>((c[off + 4] | (c[off + 5] << 8)) & 0x7FF), c[off + 6]};
            if (e.is_in())
                in = e;
            else
                out = e;
            alt = cur_alt;
        }
    }
    if (alt < 0 || !in.address || !out.address)
        return false;

    auto* iface = new EcmInterface;
    iface->usb = d;
    if (!iface->m_tx_buf) {
        delete iface;
        return false;
    }
    if (!mac_index || !read_mac(d, mac_index, iface->mac)) {
        static const u8 fallback[6] = {0x02, 0x53, 0x59, 0x4C, 0x55, 0x58}; // locally administered
        memcpy(iface->mac, fallback, 6);
    }
    if (d->control(0x01, REQ_SET_INTERFACE, static_cast<u16>(alt), static_cast<u16>(data_if), nullptr, 0) < 0 ||
        d->open_endpoint(in) < 0 || d->open_endpoint(out) < 0) {
        delete iface;
        return false;
    }
    // Directed, broadcast and all-multicast frames (IPv6 needs multicast).
    d->control(0x21, REQ_SET_PACKET_FILTER, 0x0E, intf.number, nullptr, 0);
    iface->bulk_in = in.address;
    iface->bulk_out = out.address;
    iface->out_max_packet = out.max_packet ? out.max_packet : 64;
    iface->hw = d->entry;
    for (int i = 0; i < intf.endpoint_count; i++) {
        const Endpoint& e = intf.endpoints[i];
        if (e.is_in() && e.type() == 3 && d->open_endpoint(e) == 0)
            d->start_interrupt_in(e.address, e.max_packet ? e.max_packet : 16, EcmInterface::notification, iface);
    }
    d->add_binding(EcmInterface::disconnect, iface);
    net::register_interface(iface);
    sched::spawn("cdc-ether-rx", rx_thread, iface, -1, true);
    sched::spawn("cdc-ether-tx", tx_thread, iface, -1, true);
    if (d->entry)
        snprintf(d->entry->name, sizeof(d->entry->name), "%s (USB Ethernet, %s)", d->product_name, iface->name);
    return true;
}

} // namespace usb::cdc_ether
