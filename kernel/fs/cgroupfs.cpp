// cgroup2 (mount -t cgroup2): the unified hierarchy as containers (LXC) and
// service managers see it. Every directory comes with the standard control
// files; writes are kept (and read back) but nothing is enforced yet: no
// resource limits, cgroup.procs only records what was written.

#include <fs/cgroupfs.hpp>
#include <fs/ramfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>

namespace cgroupfs {

namespace {

struct Default {
    const char* name;
    const char* text;
};

const Default g_files[] = {
    {"cgroup.procs", ""},
    {"cgroup.threads", ""},
    {"cgroup.controllers", "cpuset cpu io memory pids\n"},
    {"cgroup.subtree_control", ""},
    {"cgroup.type", "domain\n"},
    {"cgroup.events", "populated 0\nfrozen 0\n"},
    {"cgroup.freeze", "0\n"},
    {"cgroup.kill", ""},
    {"cgroup.max.depth", "max\n"},
    {"cgroup.max.descendants", "max\n"},
    {"cgroup.stat", "nr_descendants 0\nnr_dying_descendants 0\n"},
    {"cpu.max", "max 100000\n"},
    {"cpu.weight", "100\n"},
    {"cpu.stat", "usage_usec 0\nuser_usec 0\nsystem_usec 0\n"},
    {"cpuset.cpus", ""},
    {"cpuset.mems", ""},
    {"cpuset.cpus.effective", "0\n"},
    {"cpuset.mems.effective", "0\n"},
    {"io.max", ""},
    {"io.weight", "default 100\n"},
    {"memory.max", "max\n"},
    {"memory.high", "max\n"},
    {"memory.low", "0\n"},
    {"memory.min", "0\n"},
    {"memory.swap.max", "max\n"},
    {"memory.current", "0\n"},
    {"memory.events", "low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\n"},
    {"memory.stat", "anon 0\nfile 0\n"},
    {"pids.max", "max\n"},
    {"pids.current", "0\n"},
};

class CgroupDir : public ramfs::Dir {
public:
    CgroupDir() {
        mode = vfs::S_IFDIR | 0755;
        for (const Default& d : g_files) {
            auto* f = new ramfs::RegularFile;
            f->mode = vfs::S_IFREG | 0644;
            if (d.text[0])
                f->write(0, d.text, strlen(d.text));
            attach(d.name, strlen(d.name), f);
        }
    }

    // The group and its control files are on the cgroup file system's
    // device (openat2's RESOLVE_NO_XDEV: LXC opens cgroup.procs that way).
    void set_dev(u64 d) {
        dev = d;
        for (const Default& f : g_files)
            if (vfs::Inode* n = lookup(f.name, strlen(f.name)))
                n->dev = d;
    }

    long statfs(FsStats* out) override {
        *out = FsStats{0x63677270 /* CGROUP2_SUPER_MAGIC */, 4096, 0, 0, 0, 0};
        return 0;
    }

    // mkdir: a child group, with its control files.
    long create(const char* name, usize len, u32 new_mode, const char* target, vfs::Inode** out) override {
        if (!vfs::is_dir(new_mode))
            return -err::PERM; // only the kernel makes files here
        if (lookup(name, len))
            return -err::EXIST;
        auto* child = new CgroupDir;
        child->parent = this;
        child->set_dev(dev);
        attach(name, len, child);
        if (out)
            *out = child;
        return 0;
    }

    // rmdir of a child group takes its control files with it.
    long unlink(const char* name, usize len, bool remove_dir) override {
        if (remove_dir)
            if (auto* child = lookup(name, len); child && child->is_ramfs_dir())
                for (const Default& d : g_files)
                    static_cast<ramfs::Dir*>(child)->Dir::unlink(d.name, strlen(d.name), false);
        return Dir::unlink(name, len, remove_dir);
    }
};

} // namespace

// One unified hierarchy: every mount shows the same tree.
vfs::Inode* mount() {
    static CgroupDir* root = nullptr;
    if (!root) {
        root = new CgroupDir;
        root->set_dev(vfs::next_fs_dev());
        root->parent = root;
    }
    return root;
}

} // namespace cgroupfs
