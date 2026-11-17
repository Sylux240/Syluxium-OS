#include <core/log.hpp>
#include <drivers/tty.hpp>
#include <fs/devices.hpp>
#include <fs/vfs.hpp>
#include <lib/random.hpp>
#include <lib/string.hpp>

namespace devices {

namespace {

class CharDevice : public vfs::Inode {
public:
    CharDevice(u32 major, u32 minor, u32 perms = 0666) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | perms;
        rdev = (major << 8) | minor;
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }
};

class Null : public CharDevice {
public:
    Null() : CharDevice(1, 3) {}
    isize read(u64, void*, usize) override { return 0; }
    isize write(u64, const void*, usize n) override { return static_cast<isize>(n); }
};

class Zero : public CharDevice {
public:
    Zero() : CharDevice(1, 5) {}
    isize read(u64, void* buf, usize n) override {
        memset(buf, 0, n);
        return static_cast<isize>(n);
    }
    isize write(u64, const void*, usize n) override { return static_cast<isize>(n); }
};

class Full : public CharDevice {
public:
    Full() : CharDevice(1, 7) {}
    isize read(u64, void* buf, usize n) override {
        memset(buf, 0, n);
        return static_cast<isize>(n);
    }
    isize write(u64, const void*, usize) override { return -err::NOSPC; }
};

class Random : public CharDevice {
public:
    explicit Random(u32 minor) : CharDevice(1, minor) {}
    isize read(u64, void* buf, usize n) override {
        random::fill(buf, n);
        return static_cast<isize>(n);
    }
    isize write(u64, const void*, usize n) override { return static_cast<isize>(n); }
};

// Kernel log, readable by user space.
class Kmsg : public CharDevice {
public:
    Kmsg() : CharDevice(1, 11, 0644) {}
    isize read(u64, void*, usize) override { return 0; } // TODO: ring buffer of the boot log
    isize write(u64, const void* buf, usize n) override {
        kwrite(static_cast<const char*>(buf), n);
        return static_cast<isize>(n);
    }
};

} // namespace

void init() {
    vfs::mkdir_p("/dev");
    vfs::add_node("/dev/null", new Null);
    vfs::add_node("/dev/zero", new Zero);
    vfs::add_node("/dev/full", new Full);
    vfs::add_node("/dev/random", new Random(8));
    vfs::add_node("/dev/urandom", new Random(9));
    vfs::add_node("/dev/kmsg", new Kmsg);
    vfs::add_node("/dev/console", tty::console_inode());
    vfs::add_node("/dev/tty", tty::console_inode());
    vfs::add_node("/dev/tty0", tty::console_inode());
    // One real console: tty1..tty7 (what X servers open for "their" VT) are the same terminal.
    static const char* const vts[] = {"/dev/tty1", "/dev/tty2", "/dev/tty3", "/dev/tty4",
                                      "/dev/tty5", "/dev/tty6", "/dev/tty7"};
    for (const char* vt : vts)
        vfs::add_node(vt, tty::console_inode());
}

} // namespace devices
