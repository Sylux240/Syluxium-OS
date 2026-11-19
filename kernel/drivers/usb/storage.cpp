// USB mass storage (Bulk-Only Transport + SCSI): USB sticks, card readers,
// external drives. Each LUN 0 disk becomes /dev/sdX like a SATA disk, so
// partitions and FAT/ext mounting work the same. Unplugging a stick removes
// /dev/sdX (and its partitions); I/O to a still-mounted file system fails.

#include <block/block.hpp>
#include <core/log.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>

namespace usb::storage {

namespace {

struct PACKED Cbw {
    u32 signature; // "USBC"
    u32 tag;
    u32 data_length;
    u8 flags;      // bit 7: data in
    u8 lun;
    u8 cb_length;
    u8 cb[16];
};
static_assert(sizeof(Cbw) == 31);

struct PACKED Csw {
    u32 signature; // "USBS"
    u32 tag;
    u32 residue;
    u8 status;
};
static_assert(sizeof(Csw) == 13);

constexpr u32 MAX_BYTES = 64 * 1024; // the host controller's bulk bounce buffer

class Disk : public block::Device {
public:
    Disk(usb::Device* d, u8 in, u8 out) : m_usb(d), m_in(in), m_out(out) {}

    // SCSI command with an optional data phase.
    long scsi(const u8* cb, u8 cb_len, void* data, u32 length, bool data_in) {
        Cbw cbw{};
        cbw.signature = 0x43425355;
        cbw.tag = ++m_tag;
        cbw.data_length = length;
        cbw.flags = data_in ? 0x80 : 0;
        cbw.cb_length = cb_len;
        memcpy(cbw.cb, cb, cb_len);
        if (m_usb->transfer(m_out, &cbw, sizeof(cbw)) != sizeof(cbw))
            return -err::IO;
        if (length) {
            long r = m_usb->transfer(data_in ? m_in : m_out, data, length);
            if (r < 0)
                return r;
        }
        Csw csw{};
        if (m_usb->transfer(m_in, &csw, sizeof(csw)) != sizeof(csw) || csw.signature != 0x53425355 ||
            csw.tag != cbw.tag)
            return -err::IO;
        return csw.status == 0 ? 0 : -err::IO;
    }

    bool start() {
        u8 inquiry[36] = {};
        const u8 inq_cb[6] = {0x12, 0, 0, 0, sizeof(inquiry), 0};
        if (scsi(inq_cb, 6, inquiry, sizeof(inquiry), true) != 0)
            return false;
        // Vendor (8-15) + product (16-31), trimmed.
        usize n = 0;
        for (int i = 8; i < 32 && n + 1 < sizeof(model); i++) {
            char c = static_cast<char>(inquiry[i]);
            if (i == 16 && n && model[n - 1] != ' ')
                model[n++] = ' ';
            model[n++] = c >= 32 && c < 127 ? c : ' ';
        }
        while (n && model[n - 1] == ' ')
            n--;
        model[n] = 0;

        // Media may need a moment (card readers, slow sticks).
        const u8 tur[6] = {0};
        for (int i = 0; i < 20 && scsi(tur, 6, nullptr, 0, false) != 0; i++)
            sched::sleep_ms(100);

        u8 cap[8];
        const u8 cap_cb[10] = {0x25};
        if (scsi(cap_cb, 10, cap, 8, true) != 0)
            return false;
        u32 last = (u32(cap[0]) << 24) | (u32(cap[1]) << 16) | (u32(cap[2]) << 8) | cap[3];
        sector_size = (u32(cap[4]) << 24) | (u32(cap[5]) << 16) | (u32(cap[6]) << 8) | cap[7];
        sector_count = static_cast<u64>(last) + 1;
        return sector_size >= 512 && sector_size <= 4096 && sector_count > 1;
    }

    // Unplugged: from now on every request fails without touching the device.
    void unplug() { __atomic_store_n(&m_gone, true, __ATOMIC_RELEASE); }

    long read_sectors(u64 lba, u32 count, void* buf) override { return rw(lba, count, buf, false); }
    long write_sectors(u64 lba, u32 count, const void* buf) override { return rw(lba, count, const_cast<void*>(buf), true); }
    long flush() override {
        if (m_gone)
            return 0;
        LockGuard guard(m_lock);
        const u8 cb[10] = {0x35}; // SYNCHRONIZE CACHE(10)
        scsi(cb, 10, nullptr, 0, false); // optional for many sticks: ignore failure
        return 0;
    }

private:
    long rw(u64 lba, u32 count, void* buf, bool write) {
        if (m_gone || lba + count > sector_count || lba + count > 0xFFFFFFFFull)
            return -err::IO;
        LockGuard guard(m_lock);
        auto* p = static_cast<u8*>(buf);
        u32 max = MAX_BYTES / sector_size;
        while (count) {
            u32 n = min(count, max);
            u8 cb[10] = {static_cast<u8>(write ? 0x2A : 0x28), 0,
                         static_cast<u8>(lba >> 24), static_cast<u8>(lba >> 16), static_cast<u8>(lba >> 8),
                         static_cast<u8>(lba), 0, static_cast<u8>(n >> 8), static_cast<u8>(n), 0};
            if (long r = scsi(cb, 10, p, n * sector_size, !write); r != 0)
                return r;
            p += n * sector_size;
            lba += n;
            count -= n;
        }
        return 0;
    }

    usb::Device* m_usb;
    u8 m_in, m_out;
    u32 m_tag = 0;
    bool m_gone = false;
    sched::Mutex m_lock;
};

void disconnect(void* ctx) {
    auto* disk = static_cast<Disk*>(ctx);
    disk->unplug();
    block::unregister_disk(disk); // the Disk stays allocated: mounts may still refer to it
}

} // namespace

bool probe(Device* d, const Interface& intf) {
    // Mass storage, SCSI transparent command set, bulk-only transport.
    if (intf.cls != 8 || intf.subclass != 6 || intf.protocol != 0x50)
        return false;
    const Endpoint* in = nullptr;
    const Endpoint* out = nullptr;
    for (int i = 0; i < intf.endpoint_count; i++) {
        const Endpoint& e = intf.endpoints[i];
        if (e.type() == 2)
            (e.is_in() ? in : out) = &e;
    }
    if (!in || !out || d->open_endpoint(*in) != 0 || d->open_endpoint(*out) != 0)
        return false;
    auto* disk = new Disk(d, in->address, out->address);
    if (!disk->start()) {
        klog::warn("usb-storage: %s did not respond", d->product_name);
        delete disk;
        return false;
    }
    block::next_sd_name(disk->name, sizeof(disk->name));
    disk->hw = d->entry;
    disk->removable = true;
    block::register_disk(disk);
    d->add_binding(disconnect, disk);
    return true;
}

} // namespace usb::storage
