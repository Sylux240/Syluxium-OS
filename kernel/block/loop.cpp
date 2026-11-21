// Loop devices: a file used as a disk (/dev/loopN), what `mount -o loop`
// and losetup set up. Waydroid mounts Android's system.img and vendor.img
// this way. Sectors are read and written through the file; the block cache
// sits on top as for any disk.
//   /dev/loop-control: LOOP_CTL_GET_FREE, LOOP_CTL_ADD
//   /dev/loopN:        LOOP_SET_FD, LOOP_CONFIGURE, LOOP_CLR_FD,
//                      LOOP_SET_STATUS64, LOOP_GET_STATUS64, LOOP_SET_CAPACITY

#include <block/block.hpp>
#include <block/loop.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <proc/process.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>

namespace loop {

namespace {

constexpr u32 LOOP_MAJOR = 7, MISC_MAJOR = 10, LOOP_CTRL_MINOR = 237;
constexpr int MAX_LOOPS = 16;
constexpr u64 LOOP_SET_FD = 0x4C00, LOOP_CLR_FD = 0x4C01, LOOP_SET_STATUS = 0x4C02, LOOP_GET_STATUS = 0x4C03,
              LOOP_SET_STATUS64 = 0x4C04, LOOP_GET_STATUS64 = 0x4C05, LOOP_SET_CAPACITY = 0x4C07,
              LOOP_SET_DIRECT_IO = 0x4C08, LOOP_SET_BLOCK_SIZE = 0x4C09, LOOP_CONFIGURE = 0x4C0A,
              LOOP_CTL_ADD = 0x4C80, LOOP_CTL_REMOVE = 0x4C81, LOOP_CTL_GET_FREE = 0x4C82,
              BLKGETSIZE64 = 0x80081272, BLKGETSIZE = 0x1260, BLKSSZGET = 0x1268, BLKROGET = 0x125E,
              BLKFLSBUF = 0x1261, BLKRRPART = 0x125F;
constexpr u32 LO_FLAGS_READ_ONLY = 1, LO_FLAGS_AUTOCLEAR = 4, LO_FLAGS_PARTSCAN = 8;

struct LoopInfo64 { // struct loop_info64
    u64 lo_device, lo_inode, lo_rdevice, lo_offset, lo_sizelimit;
    u32 lo_number, lo_encrypt_type, lo_encrypt_key_size, lo_flags;
    u8 lo_file_name[64], lo_crypt_name[64], lo_encrypt_key[32];
    u64 lo_init[2];
};
static_assert(sizeof(LoopInfo64) == 232);
struct LoopConfig { // struct loop_config
    u32 fd, block_size;
    LoopInfo64 info;
    u64 reserved[8];
};
static_assert(sizeof(LoopConfig) == 304);

// The disk a bound loop device is.
class LoopDisk : public block::Device {
public:
    vfs::File* file = nullptr;
    u64 offset = 0;
    bool read_only = false;
    long read_sectors(u64 lba, u32 count, void* buf) override {
        isize n = file->inode->read(offset + lba * 512, buf, count * 512ull);
        if (n < 0)
            return n;
        if (static_cast<u64>(n) < count * 512ull) // past the end of the file: zeros
            memset(static_cast<u8*>(buf) + n, 0, count * 512ull - static_cast<u64>(n));
        return 0;
    }
    long write_sectors(u64 lba, u32 count, const void* buf) override {
        if (read_only)
            return -err::ROFS;
        isize n = file->inode->write(offset + lba * 512, buf, count * 512ull);
        return n < 0 ? n : 0;
    }
};

struct Loop {
    bool exists = false;
    LoopDisk* disk = nullptr; // bound
    LoopInfo64 info{};
};
Loop g_loops[MAX_LOOPS];
sched::Mutex g_lock;

void add_node(int n);

long bind(int n, int fd, const LoopInfo64* info) {
    Loop& l = g_loops[n];
    if (l.disk)
        return -err::BUSY;
    vfs::File* f = proc::fd_get(proc::current(), fd);
    if (!f)
        return -err::BADF;
    if (!vfs::is_reg(f->inode->mode)) {
        vfs::file_put(f);
        return -err::INVAL;
    }
    auto* d = new LoopDisk;
    snprintf(d->name, sizeof(d->name), "loop%d", n);
    snprintf(d->model, sizeof(d->model), "Loop device");
    d->file = f;
    d->offset = info ? info->lo_offset : 0;
    u64 size = f->inode->size > d->offset ? f->inode->size - d->offset : 0;
    if (info && info->lo_sizelimit && info->lo_sizelimit < size)
        size = info->lo_sizelimit;
    d->sector_count = size / 512;
    d->read_only = (info && (info->lo_flags & LO_FLAGS_READ_ONLY)) || (f->flags & 3) == 0 /* O_RDONLY */;
    l.info = info ? *info : LoopInfo64{};
    l.info.lo_number = static_cast<u32>(n);
    l.info.lo_inode = f->inode->ino;
    if (d->read_only)
        l.info.lo_flags |= LO_FLAGS_READ_ONLY;
    l.info.lo_flags &= ~LO_FLAGS_PARTSCAN; // no partitions on loop devices here
    l.disk = d;
    block::attach_nodeless(d);
    return 0;
}

long unbind(int n) {
    Loop& l = g_loops[n];
    if (!l.disk)
        return -err::NXIO;
    block::sync_device(l.disk);
    block::detach_nodeless(l.disk);
    vfs::file_put(l.disk->file);
    // The disk object stays (a file system mounted from it may still point at it).
    l.disk->file = nullptr;
    l.disk = nullptr;
    l.info = LoopInfo64{};
    return 0;
}

class LoopNode : public vfs::Inode {
public:
    explicit LoopNode(int n) : m_n(n) {
        ino = vfs::next_ino();
        mode = vfs::S_IFBLK | 0660;
        gid = 6; // disk
        rdev = (LOOP_MAJOR << 8) | static_cast<u32>(n);
        atime = mtime = ctime = vfs::now();
    }
    isize read(u64 offset, void* buf, usize count) override {
        LoopDisk* d = g_loops[m_n].disk;
        if (!d)
            return 0;
        u64 size = d->size_bytes();
        if (offset >= size)
            return 0;
        if (count > size - offset)
            count = size - offset;
        long r = block::read(d, offset, buf, count);
        return r < 0 ? r : static_cast<isize>(count);
    }
    isize write(u64 offset, const void* buf, usize count) override {
        LoopDisk* d = g_loops[m_n].disk;
        if (!d)
            return -err::NXIO;
        if (d->read_only)
            return -err::ROFS;
        long r = block::write(d, offset, buf, count);
        return r < 0 ? r : static_cast<isize>(count);
    }
    long ioctl_file(vfs::File*, u64 req, u64 arg) override {
        LockGuard guard(g_lock);
        Loop& l = g_loops[m_n];
        switch (req) {
        case LOOP_SET_FD:
            return bind(m_n, static_cast<int>(arg), nullptr);
        case LOOP_CONFIGURE: {
            LoopConfig c;
            if (uaccess::copy_from(&c, reinterpret_cast<const void*>(arg), sizeof(c)) < 0)
                return -err::FAULT;
            return bind(m_n, static_cast<int>(c.fd), &c.info);
        }
        case LOOP_CLR_FD:
            return unbind(m_n);
        case LOOP_SET_STATUS64: {
            if (!l.disk)
                return -err::NXIO;
            LoopInfo64 i;
            if (uaccess::copy_from(&i, reinterpret_cast<const void*>(arg), sizeof(i)) < 0)
                return -err::FAULT;
            memcpy(l.info.lo_file_name, i.lo_file_name, sizeof(i.lo_file_name));
            l.info.lo_flags = (l.info.lo_flags & LO_FLAGS_READ_ONLY) | (i.lo_flags & LO_FLAGS_AUTOCLEAR);
            if (i.lo_offset != l.disk->offset || i.lo_sizelimit) {
                l.disk->offset = i.lo_offset;
                u64 size = l.disk->file->inode->size > i.lo_offset ? l.disk->file->inode->size - i.lo_offset : 0;
                if (i.lo_sizelimit && i.lo_sizelimit < size)
                    size = i.lo_sizelimit;
                l.disk->sector_count = size / 512;
                l.info.lo_offset = i.lo_offset;
                l.info.lo_sizelimit = i.lo_sizelimit;
            }
            return 0;
        }
        case LOOP_GET_STATUS64:
            if (!l.disk)
                return -err::NXIO;
            return uaccess::copy_to(reinterpret_cast<void*>(arg), &l.info, sizeof(l.info)) < 0 ? -err::FAULT : 0;
        case LOOP_SET_STATUS:
        case LOOP_GET_STATUS:
            return -err::INVAL; // the old 32-bit layout: util-linux uses the 64-bit calls
        case LOOP_SET_CAPACITY:
        case LOOP_SET_DIRECT_IO:
        case LOOP_SET_BLOCK_SIZE:
        case BLKFLSBUF:
        case BLKRRPART:
            return l.disk ? 0 : -err::NXIO;
        case BLKGETSIZE64: {
            u64 size = l.disk ? l.disk->size_bytes() : 0;
            return uaccess::put(reinterpret_cast<u64*>(arg), size);
        }
        case BLKGETSIZE: {
            u64 sectors = l.disk ? l.disk->sector_count : 0;
            return uaccess::put(reinterpret_cast<u64*>(arg), sectors);
        }
        case BLKSSZGET:
            return uaccess::put(reinterpret_cast<i32*>(arg), 512);
        case BLKROGET:
            return uaccess::put(reinterpret_cast<i32*>(arg), l.disk && l.disk->read_only ? 1 : 0);
        default:
            return -err::NOTTY;
        }
    }

private:
    int m_n;
};

class LoopControl : public vfs::Inode {
public:
    LoopControl() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0660;
        gid = 6;
        rdev = (MISC_MAJOR << 8) | LOOP_CTRL_MINOR;
        atime = mtime = ctime = vfs::now();
    }
    long ioctl_file(vfs::File*, u64 req, u64 arg) override {
        LockGuard guard(g_lock);
        switch (req) {
        case LOOP_CTL_GET_FREE:
            for (int n = 0; n < MAX_LOOPS; n++) {
                if (!g_loops[n].exists)
                    add_node(n);
                if (!g_loops[n].disk)
                    return n;
            }
            return -err::NOSPC;
        case LOOP_CTL_ADD: {
            int n = static_cast<int>(arg);
            if (n < 0 || n >= MAX_LOOPS)
                return -err::INVAL;
            if (g_loops[n].exists)
                return -err::EXIST;
            add_node(n);
            return n;
        }
        case LOOP_CTL_REMOVE: {
            int n = static_cast<int>(arg);
            if (n < 0 || n >= MAX_LOOPS || !g_loops[n].exists)
                return -err::NODEV;
            return g_loops[n].disk ? -err::BUSY : 0; // the node stays (cheap), like a fresh one
        }
        default:
            return -err::NOTTY;
        }
    }
};

void add_node(int n) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/loop%d", n);
    vfs::add_node(path, new LoopNode(n));
    g_loops[n].exists = true;
}

} // namespace

void unmounted(const char* source) {
    if (strncmp(source, "/dev/loop", 9) != 0 || source[9] < '0' || source[9] > '9')
        return;
    int n = 0;
    for (const char* c = source + 9; *c >= '0' && *c <= '9'; c++)
        n = n * 10 + (*c - '0');
    LockGuard guard(g_lock);
    if (n < MAX_LOOPS && g_loops[n].disk && (g_loops[n].info.lo_flags & LO_FLAGS_AUTOCLEAR))
        unbind(n);
}

void create_nodes() {
    vfs::add_node("/dev/loop-control", new LoopControl);
    for (int n = 0; n < 8; n++)
        add_node(n);
}

} // namespace loop
