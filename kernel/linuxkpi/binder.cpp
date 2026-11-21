// LinuxKPI: the Syluxium side of the binder driver (drivers/android/binder.c):
// who the current process is (ids, task, mm), and binderfs, the file system
// Android containers (Waydroid) mount to get their own binder devices:
//   mount -t binder binder /dev/binderfs
// gives binder, hwbinder and vndbinder (new, separate contexts per mount),
// binder-control (BINDER_CTL_ADD creates more) and features/.

#include <fs/ramfs.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <proc/process.hpp>
#include <proc/uaccess.hpp>
#include <sched/thread.hpp>

extern "C" {
struct task_struct;
struct mm_struct;
int lkpi_binder_add_device(const char* name, int* minor); // binder.c
void put_task_struct(task_struct* t);
void mmput(mm_struct* mm);
void mmdrop(mm_struct* mm);
void* lkpi_kzalloc(usize size, unsigned int flags);
}

namespace linuxkpi {
vfs::Inode* make_char_node(u32 major, u32 minor, u32 perms); // file.cpp
}

namespace {

// Matches struct mm_struct in linux/lkpi_binder.h.
struct LkpiMm {
    int mm_count;
    int mm_users;
    void* lkpi_process;
};

// The fields of struct task_struct this file needs (linux/kernel.h).
struct LkpiTaskView {
    char comm[16];
    int pid;
    int tgid;
    volatile long state;
    void* lkpi_thread;
    void* mm;
    unsigned int flags;
    int exit_code;
    void* group_leader;
    int lkpi_usage;
    void* lkpi_process;
};

} // namespace

extern "C" {

void lkpi_current_ids(unsigned int ids[8]) {
    const proc::Cred& c = proc::cred();
    ids[0] = c.ruid, ids[1] = c.euid, ids[2] = c.suid, ids[3] = c.fsuid;
    ids[4] = c.rgid, ids[5] = c.egid, ids[6] = c.sgid, ids[7] = c.fsgid;
}

// A task's effective uid: its process's (binder: the sender of a call).
unsigned int lkpi_task_euid(task_struct* t) {
    auto* v = reinterpret_cast<LkpiTaskView*>(t);
    auto* p = static_cast<proc::Process*>(v->lkpi_process);
    if (!p && v->lkpi_thread)
        p = static_cast<sched::Thread*>(v->lkpi_thread)->process;
    return p ? p->cred.euid : 0;
}

// The current process's mm (one per process; it holds one reference of each kind).
mm_struct* lkpi_current_mm(void) {
    proc::Process* p = proc::current();
    if (!p)
        return nullptr;
    if (!p->lkpi_mm) {
        auto* mm = static_cast<LkpiMm*>(lkpi_kzalloc(sizeof(LkpiMm), 1 /* GFP_KERNEL */));
        mm->mm_count = 1;
        mm->mm_users = 1;
        mm->lkpi_process = p;
        void* expected = nullptr;
        if (!__atomic_compare_exchange_n(&p->lkpi_mm, &expected, static_cast<void*>(mm), false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            mmdrop(reinterpret_cast<mm_struct*>(mm));
    }
    return static_cast<mm_struct*>(p->lkpi_mm);
}

} // extern "C"

namespace linuxkpi {

// The process is being freed: its task and mm outlive it only as long as
// Linux code holds references.
void process_gone(proc::Process* p) {
    if (p->lkpi_task) {
        reinterpret_cast<LkpiTaskView*>(p->lkpi_task)->lkpi_process = nullptr;
        put_task_struct(static_cast<task_struct*>(p->lkpi_task));
        p->lkpi_task = nullptr;
    }
    if (p->lkpi_mm) {
        auto* mm = static_cast<LkpiMm*>(p->lkpi_mm);
        mm->lkpi_process = nullptr;
        mmput(reinterpret_cast<mm_struct*>(mm));
        mmdrop(reinterpret_cast<mm_struct*>(mm));
        p->lkpi_mm = nullptr;
    }
}

} // namespace linuxkpi

// ---------------------------------------------------------------- binderfs

namespace {

constexpr u32 MISC_MAJOR = 10;
constexpr usize BINDERFS_NAME_MAX = 255;
struct BinderfsDevice { // uapi/linux/android/binderfs.h
    char name[BINDERFS_NAME_MAX + 1];
    u32 major;
    u32 minor;
};
constexpr u64 BINDER_CTL_ADD = 0xC1086201; // _IOWR('b', 1, struct binderfs_device)

long add_device(ramfs::Dir* dir, const char* name, u32* minor_out) {
    usize len = strlen(name);
    if (!len || len > BINDERFS_NAME_MAX || !strcmp(name, ".") || !strcmp(name, ".."))
        return -err::INVAL;
    for (usize i = 0; i < len; i++)
        if (name[i] == '/')
            return -err::INVAL;
    if (dir->lookup(name, len))
        return -err::EXIST;
    int minor = 0;
    if (int r = lkpi_binder_add_device(name, &minor); r < 0)
        return r;
    vfs::Inode* node = linuxkpi::make_char_node(MISC_MAJOR, static_cast<u32>(minor), 0666);
    node->dev = dir->dev;
    dir->attach(name, len, node);
    if (minor_out)
        *minor_out = static_cast<u32>(minor);
    return 0;
}

// binder-control: BINDER_CTL_ADD makes a new device in this binderfs.
class BinderControl : public vfs::Inode {
public:
    explicit BinderControl(ramfs::Dir* dir) : m_dir(dir) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0600;
        rdev = (MISC_MAJOR << 8) | 0xFF;
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }
    long ioctl_file(vfs::File*, u64 request, u64 arg) override {
        if (request != BINDER_CTL_ADD)
            return -err::INVAL;
        if (!proc::capable())
            return -err::PERM;
        BinderfsDevice d;
        if (uaccess::copy_from(&d, reinterpret_cast<const void*>(arg), sizeof(d)) < 0)
            return -err::FAULT;
        d.name[BINDERFS_NAME_MAX] = 0;
        u32 minor = 0;
        if (long r = add_device(m_dir, d.name, &minor); r < 0)
            return r;
        d.major = MISC_MAJOR;
        d.minor = minor;
        return uaccess::copy_to(reinterpret_cast<void*>(arg), &d, sizeof(d)) < 0 ? -err::FAULT : 0;
    }

private:
    ramfs::Dir* m_dir;
};

// features/<name>: "1" (what this binder supports).
class FeatureFile : public vfs::Inode {
public:
    FeatureFile() {
        ino = vfs::next_ino();
        mode = vfs::S_IFREG | 0444;
        size = 2;
        atime = mtime = ctime = vfs::now();
    }
    isize read(u64 offset, void* buf, usize count) override {
        static const char text[] = "1\n";
        if (offset >= 2)
            return 0;
        usize n = count < 2 - offset ? count : 2 - offset;
        memcpy(buf, text + offset, n);
        return static_cast<isize>(n);
    }
};

} // namespace

namespace linuxkpi {

// A new binderfs instance (mount -t binder): binder, hwbinder, vndbinder,
// binder-control, features/.
vfs::Inode* binderfs_mount() {
    static u64 instance = 0;
    auto* root = new ramfs::Dir;
    root->mode = vfs::S_IFDIR | 0755;
    root->dev = 0x0B1D0000 + __atomic_add_fetch(&instance, 1, __ATOMIC_RELAXED);
    static const char* const devices[] = {"binder", "hwbinder", "vndbinder"};
    for (const char* name : devices)
        add_device(root, name, nullptr);
    auto* control = new BinderControl(root);
    control->dev = root->dev;
    root->attach("binder-control", 14, control);
    auto* features = new ramfs::Dir;
    features->mode = vfs::S_IFDIR | 0755;
    static const char* const supported[] = {"oneway_spam_detection", "extended_error"};
    for (const char* f : supported)
        features->attach(f, strlen(f), new FeatureFile);
    root->attach("features", 8, features);
    return root;
}

} // namespace linuxkpi
