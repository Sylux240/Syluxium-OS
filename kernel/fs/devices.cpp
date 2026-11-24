#include <arch/x86_64/protect.hpp>
#include <block/loop.hpp>
#include <video/splash.hpp>
#include <core/log.hpp>
#include <drivers/pty.hpp>
#include <proc/process.hpp>
#include <drivers/tty.hpp>
#include <fs/devices.hpp>
#include <fs/fuse.hpp>
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
        protect::UserAccess window; // buf is the caller's (user) buffer
        memset(buf, 0, n);
        return static_cast<isize>(n);
    }
    isize write(u64, const void*, usize n) override { return static_cast<isize>(n); }
};

class Full : public CharDevice {
public:
    Full() : CharDevice(1, 7) {}
    isize read(u64, void* buf, usize n) override {
        protect::UserAccess window; // buf is the caller's (user) buffer
        memset(buf, 0, n);
        return static_cast<isize>(n);
    }
    isize write(u64, const void*, usize) override { return -err::NOSPC; }
};

class Random : public CharDevice {
public:
    explicit Random(u32 minor) : CharDevice(1, minor) {}
    isize read(u64, void* buf, usize n) override {
        protect::UserAccess window; // buf is the caller's (user) buffer
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
    // Into the kernel log (dmesg, the boot log on the stick), one line per
    // write as on Linux; an optional "<level>" prefix is dropped.
    isize write(u64, const void* buf, usize n) override {
        char line[512];
        usize len = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        {
            protect::UserAccess window; // buf is the caller's (user) buffer
            memcpy(line, buf, len);
        }
        usize start = 0;
        if (len > 2 && line[0] == '<') {
            usize i = 1;
            while (i < len && i < 5 && line[i] >= '0' && line[i] <= '9')
                i++;
            if (i < len && line[i] == '>')
                start = i + 1;
        }
        while (len > start && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        line[len] = 0;
        if (len > start)
            klog::info("%s", line + start);
        return static_cast<isize>(n);
    }
};

} // namespace

// /dev/tty: opens the caller's controlling terminal (a pseudo-terminal in
// xterm, the console for programs started from it).
class DevTty : public vfs::Inode {
public:
    DevTty() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0666;
        rdev = (5u << 8) | 0;
        atime = mtime = ctime = vfs::now();
    }
    vfs::Inode* open_redirect() override {
        proc::Process* p = proc::current();
        return p && p->ctty ? p->ctty : tty::console_inode();
    }
};

void init() {
    vfs::mkdir_p("/dev");
    vfs::add_node("/dev/null", new Null);
    splash::create_device(); // /dev/splash: the login screen ends the boot splash
    vfs::add_node("/dev/zero", new Zero);
    vfs::add_node("/dev/full", new Full);
    loop::create_nodes(); // /dev/loop-control, /dev/loopN (mount -o loop)
    fuse::create_device(); // /dev/fuse (FUSE file systems: AppImages, sshfs)
    vfs::add_node("/dev/random", new Random(8));
    vfs::add_node("/dev/urandom", new Random(9));
    vfs::add_node("/dev/kmsg", new Kmsg);
    vfs::add_node("/dev/console", tty::console_inode());
    vfs::add_node("/dev/tty", new DevTty);
    vfs::add_node("/dev/tty0", tty::console_inode());
    pty::init(); // /dev/ptmx, /dev/pts
    // The usual symlinks (udev makes them on Linux): bash's <(...) process
    // substitution and scripts reading /dev/stdin need them.
    {
        static const char* const links[][2] = {
            {"fd", "/proc/self/fd"}, {"stdin", "/proc/self/fd/0"}, {"stdout", "/proc/self/fd/1"},
            {"stderr", "/proc/self/fd/2"}};
        vfs::Inode* dev;
        if (vfs::resolve("/dev", nullptr, true, &dev) == 0)
            for (const auto& l : links)
                dev->create(l[0], strlen(l[0]), vfs::S_IFLNK | 0777, l[1], nullptr);
    }
    // One real console: tty1..tty7 (what X servers open for "their" VT) are the same terminal.
    static const char* const vts[] = {"/dev/tty1", "/dev/tty2", "/dev/tty3", "/dev/tty4",
                                      "/dev/tty5", "/dev/tty6", "/dev/tty7"};
    for (const char* vt : vts)
        vfs::add_node(vt, tty::console_inode());
}

} // namespace devices
