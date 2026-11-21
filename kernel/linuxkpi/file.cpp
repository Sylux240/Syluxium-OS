// LinuxKPI: the Syluxium side of Linux files. A LinuxFileNode wraps one
// struct file (anonymous files: dma-buf, sync_file, DRM sync objects); a
// LinuxCharNode is a /dev node whose every open creates a struct file through
// the driver's file_operations (DRM's /dev/dri/cardN). System calls forward
// to f_op->read/write/unlocked_ioctl/poll/release (glue/file.c).

#include <video/splash.hpp>
#include <video/console.hpp>
#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/vmm.hpp>
#include <proc/memory.hpp>
#include <proc/process.hpp>

extern "C" {
struct file;
long lkpi_file_read(file* f, char* buf, usize n, long long* pos);
long lkpi_file_write(file* f, const char* buf, usize n, long long* pos);
long lkpi_file_ioctl(file* f, unsigned int cmd, unsigned long arg);
unsigned int lkpi_file_poll(file* f);
void fput(file* f);
file* get_file(file* f);
file* lkpi_chrdev_open(unsigned int major, unsigned int minor, unsigned int flags, int* err);
long lkpi_file_mmap(file* f, unsigned long offset, unsigned long len, bool writable, unsigned long* handle);
long lkpi_vma_phys(unsigned long off, u64* phys);
void lkpi_vma_ref(unsigned long off, int delta);
int lkpi_vma_cache(unsigned long off);
}

namespace {
u32 cache_flags(u64 off) {
    switch (lkpi_vma_cache(off)) {
    case 1: return vmm::CACHE_WC;
    case 2: return vmm::CACHE_UC;
    default: return 0;
    }
}
} // namespace

namespace {

// Forwarding, shared by both node kinds (the struct file is per open file).
isize do_read(file* lf, vfs::File* f, void* buf, usize n) {
    if (!lf)
        return -err::NODEV;
    long long pos = static_cast<long long>(f->offset);
    long r = lkpi_file_read(lf, static_cast<char*>(buf), n, &pos);
    if (r > 0)
        f->offset = static_cast<u64>(pos);
    return r;
}
isize do_write(file* lf, vfs::File* f, const void* buf, usize n) {
    if (!lf)
        return -err::NODEV;
    long long pos = static_cast<long long>(f->offset);
    long r = lkpi_file_write(lf, static_cast<const char*>(buf), n, &pos);
    if (r > 0)
        f->offset = static_cast<u64>(pos);
    return r;
}

// One Linux file, the same struct file for every descriptor sharing it.
class LinuxFileNode : public vfs::Inode {
public:
    explicit LinuxFileNode(file* lf) : m_file(lf) {
        ino = vfs::next_ino();
        mode = vfs::S_IFREG | 0600;
        atime = mtime = ctime = vfs::now();
    }
    ~LinuxFileNode() override { fput(m_file); }
    void* linux_file(vfs::File*) override { return m_file; }
    long mmap_prepare(vfs::File* f, u64 offset, u64 len, bool writable, u64* new_offset) override {
        unsigned long h;
        long r = lkpi_file_mmap(static_cast<file*>(linux_file(f)), offset, len, writable, &h);
        if (r == 0)
            *new_offset = h;
        return r;
    }
    long mmap_phys(u64 offset, u64* phys) override { return lkpi_vma_phys(offset, phys); }
    void mmap_range_ref(u64 offset, int delta) override { lkpi_vma_ref(offset, delta); }
    u32 mmap_cache_flags(u64 offset) override { return cache_flags(offset); }
    bool seekable() const override { return false; }
    isize read_file(vfs::File* f, u64, void* buf, usize n) override { return do_read(m_file, f, buf, n); }
    isize write_file(vfs::File* f, u64, const void* buf, usize n) override { return do_write(m_file, f, buf, n); }
    long ioctl_file(vfs::File*, u64 req, u64 arg) override {
        return lkpi_file_ioctl(m_file, static_cast<unsigned int>(req), arg);
    }
    u16 poll(vfs::File*) override { return static_cast<u16>(lkpi_file_poll(m_file)); }
    void on_release(vfs::File*) override { m_released = true; }
    bool released() const { return m_released; }

private:
    file* m_file;
    bool m_released = false;
};

// A /dev node of a Linux character driver: each open is a new struct file.
class LinuxCharNode : public vfs::Inode {
public:
    LinuxCharNode(u32 major, u32 minor, u32 perms) : m_major(major), m_minor(minor) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | perms;
        rdev = (major << 8) | (minor & 0xFF) | ((minor & ~0xFFu) << 12);
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }
    void* linux_file(vfs::File* f) override { return f->private_data; }
    long mmap_prepare(vfs::File* f, u64 offset, u64 len, bool writable, u64* new_offset) override {
        unsigned long h;
        long r = lkpi_file_mmap(static_cast<file*>(linux_file(f)), offset, len, writable, &h);
        if (r == 0)
            *new_offset = h;
        return r;
    }
    long mmap_phys(u64 offset, u64* phys) override { return lkpi_vma_phys(offset, phys); }
    void mmap_range_ref(u64 offset, int delta) override { lkpi_vma_ref(offset, delta); }
    u32 mmap_cache_flags(u64 offset) override { return cache_flags(offset); }
    void on_open(vfs::File* f) override {
        int err = 0;
        if (is_display()) {
            // A display server takes the screen: the text console stops
            // drawing over it until the last handle is closed (as for /dev/fb0).
            splash::handover();
            if (__atomic_add_fetch(&m_display_owners, 1, __ATOMIC_ACQ_REL) == 1)
                console::set_graphics(true);
        }
        f->private_data = lkpi_chrdev_open(m_major, m_minor, f->flags, &err);
    }
    void on_release(vfs::File* f) override {
        if (f->private_data)
            fput(static_cast<file*>(f->private_data));
        f->private_data = nullptr;
        if (is_display() && __atomic_sub_fetch(&m_display_owners, 1, __ATOMIC_ACQ_REL) == 0 && !splash::active())
            console::set_graphics(false);
    }
    // DRM primary nodes (card0..): not render nodes (minor 128+), which can't scan out.
    bool is_display() const { return m_major == 226 && m_minor < 64; }
    u32 m_display_owners = 0;
    isize read_file(vfs::File* f, u64, void* buf, usize n) override {
        return do_read(static_cast<file*>(f->private_data), f, buf, n);
    }
    isize write_file(vfs::File* f, u64, const void* buf, usize n) override {
        return do_write(static_cast<file*>(f->private_data), f, buf, n);
    }
    long ioctl_file(vfs::File* f, u64 req, u64 arg) override {
        auto* lf = static_cast<file*>(f->private_data);
        return lf ? lkpi_file_ioctl(lf, static_cast<unsigned int>(req), arg) : -err::NODEV;
    }
    u16 poll(vfs::File* f) override {
        auto* lf = static_cast<file*>(f->private_data);
        return lf ? static_cast<u16>(lkpi_file_poll(lf)) : ::poll::POLLERR;
    }

private:
    u32 m_major, m_minor;
};

// Placeholder for descriptors reserved by get_unused_fd_flags().
class ReservedNode : public vfs::Inode {
public:
    ReservedNode() { mode = vfs::S_IFREG; }
};
ReservedNode g_reserved;

} // namespace

extern "C" {

int lkpi_vfs_fd_reserve(unsigned int flags) {
    vfs::File* f = vfs::open_inode(&g_reserved, vfs::O_RDWR);
    long fd = proc::fd_install(proc::current(), f, 0, flags & vfs::O_CLOEXEC);
    if (fd < 0)
        vfs::file_put(f);
    return static_cast<int>(fd);
}

void lkpi_vfs_fd_release(unsigned int fd) { proc::fd_close(proc::current(), static_cast<int>(fd)); }

// The descriptor takes over the caller's reference to `lf`.
void lkpi_vfs_fd_install(unsigned int fd, file* lf) {
    auto* node = new LinuxFileNode(lf);
    vfs::File* f = vfs::open_inode(node, vfs::O_RDWR);
    char name[32];
    snprintf(name, sizeof(name), "anon_inode:[%lu]", node->ino);
    vfs::file_set_path(f, name);
    proc::fd_replace(proc::current(), static_cast<int>(fd), f);
}

file* lkpi_vfs_fget(unsigned int fd) {
    vfs::File* f = proc::fd_get(proc::current(), static_cast<int>(fd));
    if (!f)
        return nullptr;
    file* lf = static_cast<file*>(f->inode->linux_file(f));
    if (lf)
        get_file(lf);
    vfs::file_put(f);
    return lf;
}

int lkpi_chrdev_node(const char* path, unsigned int major, unsigned int minor, unsigned short perms) {
    // Subdirectories like /dev/dri are created on demand.
    char dir[128];
    snprintf(dir, sizeof(dir), "%s", path);
    char* slash = nullptr;
    for (char* c = dir; *c; c++)
        if (*c == '/')
            slash = c;
    if (slash && slash != dir) {
        *slash = 0;
        vfs::mkdir_p(dir);
    }
    vfs::add_node(path, new LinuxCharNode(major, minor, perms & 07777));
    return 0;
}

void lkpi_chrdev_node_remove(const char* path) { vfs::remove_node(path); }

} // extern "C"

// glue/file.c: drivers revoke user mappings (unmap_mapping_range).
extern "C" void lkpi_zap_user_range(u64 offset, u64 len) { proc::zap_device_range(offset, len); }

// glue/lib.c: a Linux character device (DRM card/render node) of a PCI device.
#include <fs/procfs.hpp>
extern "C" void lkpi_sysfs_char_device(unsigned int major, unsigned int minor, const char* devname,
                                       const char* pci_bus_id) {
    sysfs::add_char_device(major, minor, devname, pci_bus_id);
}
