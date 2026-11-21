// /sys/class/net/<if>/ (address, operstate, carrier, speed, mtu, flags,
// statistics/*, ...) as Linux has it, for the tools and libraries that read
// interfaces there (NetworkManager-like tools, systemd's net helpers,
// Syluxium's Network Connections), and /proc/sys/net/ipv4/conf/<if>/ with
// Syluxium's switch for the kernel's DHCP client (syluxium_dhcp: "1" on,
// "0" off for a static address, "restart" asks the network again now).
// The directories list the interfaces as they are at the time of reading.

#include <fs/genfs.hpp>
#include <lib/format.hpp>
#include <net/internal.hpp>
#include <proc/process.hpp>

namespace net {

namespace {

using genfs::TextBuffer;
constexpr usize MAX_IFDIRS = 32;

bool name_is(const char* name, usize len, const char* s) { return strlen(s) == len && memcmp(name, s, len) == 0; }

void fill(vfs::DirEntry* out, vfs::Inode* n, const char* name) {
    out->ino = n->ino;
    out->type = vfs::is_dir(n->mode) ? vfs::DT_DIR : vfs::is_lnk(n->mode) ? vfs::DT_LNK : vfs::DT_REG;
    snprintf(out->name, sizeof(out->name), "%s", name);
}

// A directory with a fixed set of entries.
class FixedDir : public vfs::Inode {
public:
    FixedDir() {
        ino = vfs::next_ino();
        mode = vfs::S_IFDIR | 0755;
        nlink = 2;
        atime = mtime = ctime = vfs::now();
    }
    void add(const char* name, vfs::Inode* node) {
        if (m_count < MAX)
            m_entries[m_count++] = {name, node};
    }
    vfs::Inode* lookup(const char* name, usize len) override {
        for (int i = 0; i < m_count; i++)
            if (name_is(name, len, m_entries[i].name))
                return m_entries[i].inode;
        return nullptr;
    }
    long readdir(u64 index, vfs::DirEntry* out) override {
        if (index < 2) {
            out->ino = ino;
            out->type = vfs::DT_DIR;
            snprintf(out->name, sizeof(out->name), "%s", index == 0 ? "." : "..");
            return 1;
        }
        if (index - 2 >= static_cast<u64>(m_count))
            return 0;
        fill(out, m_entries[index - 2].inode, m_entries[index - 2].name);
        return 1;
    }
    isize read(u64, void*, usize) override { return -err::ISDIR; }

private:
    static constexpr int MAX = 24;
    struct Entry {
        const char* name;
        vfs::Inode* inode;
    } m_entries[MAX];
    int m_count = 0;
};

// One subdirectory per interface, made on first use.
class InterfacesDir : public vfs::Inode {
public:
    using Make = vfs::Inode* (*)(Interface*);
    explicit InterfacesDir(Make make) : m_make(make) {
        ino = vfs::next_ino();
        mode = vfs::S_IFDIR | 0755;
        nlink = 2;
        atime = mtime = ctime = vfs::now();
    }
    vfs::Inode* lookup(const char* name, usize len) override {
        for (u32 i = 1; Interface* f = interface_by_index(i); i++)
            if (name_is(name, len, f->name))
                return dir(f);
        return nullptr;
    }
    long readdir(u64 index, vfs::DirEntry* out) override {
        if (index < 2) {
            out->ino = ino;
            out->type = vfs::DT_DIR;
            snprintf(out->name, sizeof(out->name), "%s", index == 0 ? "." : "..");
            return 1;
        }
        Interface* f = interface_by_index(static_cast<u32>(index - 1));
        if (!f)
            return 0;
        fill(out, dir(f), f->name);
        return 1;
    }
    isize read(u64, void*, usize) override { return -err::ISDIR; }

private:
    vfs::Inode* dir(Interface* f) {
        if (f->index >= MAX_IFDIRS)
            return nullptr;
        if (!m_dirs[f->index])
            m_dirs[f->index] = m_make(f);
        return m_dirs[f->index];
    }
    Make m_make;
    vfs::Inode* m_dirs[MAX_IFDIRS] = {};
};

const Interface* IF(const void* c) { return static_cast<const Interface*>(c); }
bool carrier(const Interface* f) { return (f->flags & IFF_UP) && f->link_up && !f->detached; }

bool gen_address(TextBuffer& o, const void* c) {
    const u8* m = IF(c)->mac;
    o.printf("%02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    return true;
}
bool gen_broadcast(TextBuffer& o, const void* c) {
    o.printf((IF(c)->flags & IFF_LOOPBACK) ? "00:00:00:00:00:00\n" : "ff:ff:ff:ff:ff:ff\n");
    return true;
}
bool gen_addr_len(TextBuffer& o, const void*) { o.printf("6\n"); return true; }
bool gen_ifindex(TextBuffer& o, const void* c) { o.printf("%u\n", IF(c)->index); return true; }
bool gen_mtu(TextBuffer& o, const void* c) { o.printf("%u\n", IF(c)->mtu); return true; }
bool gen_type(TextBuffer& o, const void* c) { o.printf("%u\n", (IF(c)->flags & IFF_LOOPBACK) ? 772u : 1u); return true; }
bool gen_txqlen(TextBuffer& o, const void* c) { o.printf("%u\n", (IF(c)->flags & IFF_LOOPBACK) ? 0u : 1000u); return true; }
bool gen_flags(TextBuffer& o, const void* c) {
    const Interface* f = IF(c);
    u32 fl = (f->flags & ~IFF_RUNNING) | (carrier(f) ? IFF_RUNNING : 0);
    o.printf("0x%x\n", fl);
    return true;
}
bool gen_operstate(TextBuffer& o, const void* c) {
    const Interface* f = IF(c);
    o.printf("%s\n", (f->flags & IFF_LOOPBACK) ? "unknown" : carrier(f) ? "up" : "down");
    return true;
}
bool gen_carrier(TextBuffer& o, const void* c) { o.printf("%d\n", carrier(IF(c)) ? 1 : 0); return true; }
bool gen_speed(TextBuffer& o, const void* c) {
    auto* f = const_cast<Interface*>(IF(c));
    u32 mbps;
    bool full;
    if (carrier(f) && f->link_settings(&mbps, &full))
        o.printf("%u\n", mbps);
    else
        o.printf("-1\n");
    return true;
}
bool gen_duplex(TextBuffer& o, const void* c) {
    auto* f = const_cast<Interface*>(IF(c));
    u32 mbps;
    bool full;
    if (carrier(f) && f->link_settings(&mbps, &full))
        o.printf("%s\n", full ? "full" : "half");
    else
        o.printf("unknown\n");
    return true;
}
bool gen_uevent(TextBuffer& o, const void* c) {
    o.printf("INTERFACE=%s\nIFINDEX=%u\n", IF(c)->name, IF(c)->index);
    return true;
}
#define STAT(fn, expr) bool fn(TextBuffer& o, const void* c) { o.printf("%lu\n", static_cast<u64>(expr)); return true; }
STAT(gen_rx_bytes, IF(c)->rx_bytes)
STAT(gen_tx_bytes, IF(c)->tx_bytes)
STAT(gen_rx_packets, IF(c)->rx_packets)
STAT(gen_tx_packets, IF(c)->tx_packets)
STAT(gen_rx_dropped, IF(c)->rx_dropped)
STAT(gen_tx_errors, IF(c)->tx_errors)
STAT(gen_zero, 0)
#undef STAT

vfs::Inode* make_sys_dir(Interface* f) {
    auto* d = new FixedDir;
    auto file = [&](const char* name, genfs::GeneratedFile::Generator g) { d->add(name, new genfs::GeneratedFile(g, f)); };
    file("address", gen_address);
    file("broadcast", gen_broadcast);
    file("addr_len", gen_addr_len);
    file("ifindex", gen_ifindex);
    file("iflink", gen_ifindex);
    file("mtu", gen_mtu);
    file("type", gen_type);
    file("tx_queue_len", gen_txqlen);
    file("flags", gen_flags);
    file("operstate", gen_operstate);
    file("carrier", gen_carrier);
    file("speed", gen_speed);
    file("duplex", gen_duplex);
    file("uevent", gen_uevent);
    auto* st = new FixedDir;
    auto stat = [&](const char* name, genfs::GeneratedFile::Generator g) { st->add(name, new genfs::GeneratedFile(g, f)); };
    stat("rx_bytes", gen_rx_bytes);
    stat("tx_bytes", gen_tx_bytes);
    stat("rx_packets", gen_rx_packets);
    stat("tx_packets", gen_tx_packets);
    stat("rx_errors", gen_zero);
    stat("tx_errors", gen_tx_errors);
    stat("rx_dropped", gen_rx_dropped);
    stat("tx_dropped", gen_zero);
    stat("multicast", gen_zero);
    stat("collisions", gen_zero);
    d->add("statistics", st);
    return d;
}

// /proc/sys/net/ipv4/conf/<if>/syluxium_dhcp
class DhcpSwitch : public vfs::Inode {
public:
    explicit DhcpSwitch(Interface* f) : m_iface(f) {
        ino = vfs::next_ino();
        mode = vfs::S_IFREG | 0644;
        atime = mtime = ctime = vfs::now();
    }
    isize read(u64 offset, void* buf, usize count) override {
        const char* text = dhcp_enabled(m_iface) ? "1\n" : "0\n";
        usize n = strlen(text);
        if (offset >= n)
            return 0;
        n = min<usize>(n - offset, count);
        memcpy(buf, text + offset, n);
        return static_cast<isize>(n);
    }
    isize write(u64, const void* buf, usize count) override {
        if (!proc::capable())
            return -err::PERM;
        char text[16] = {};
        memcpy(text, buf, min<usize>(count, sizeof(text) - 1));
        if (text[0] == '0')
            dhcp_set_enabled(m_iface, false);
        else if (text[0] == '1')
            dhcp_set_enabled(m_iface, true);
        else if (memcmp(text, "restart", 7) == 0)
            dhcp_restart(m_iface);
        else
            return -err::INVAL;
        return static_cast<isize>(count);
    }
    long truncate(u64) override { return 0; }

private:
    Interface* m_iface;
};

bool gen_forwarding(TextBuffer& o, const void*) { o.printf("0\n"); return true; }

vfs::Inode* make_conf_dir(Interface* f) {
    auto* d = new FixedDir;
    if (!(f->flags & IFF_LOOPBACK))
        d->add("syluxium_dhcp", new DhcpSwitch(f));
    d->add("forwarding", new genfs::GeneratedFile(gen_forwarding, f));
    return d;
}

} // namespace

void sysnet_init() {
    vfs::mkdir_p("/sys/class", 0755);
    vfs::add_node("/sys/class/net", new InterfacesDir(make_sys_dir));
    vfs::mkdir_p("/proc/sys/net/ipv4", 0555);
    vfs::add_node("/proc/sys/net/ipv4/conf", new InterfacesDir(make_conf_dir));
}

} // namespace net
