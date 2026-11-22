#include <arch/x86_64/protect.hpp>
#include <fs/ramfs.hpp>
#include <fs/inotify.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <proc/uaccess.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace vfs {

namespace {

ramfs::Dir* g_ram_root = nullptr; // the RAM file system created at boot
Inode* g_root = nullptr;          // "/": the RAM root, or a disk after set_root()
char g_root_source[64] = "ramfs", g_root_type[16] = "ramfs";
u64 g_next_ino = 1;
constexpr int MAX_SYMLINK_DEPTH = 40;
constexpr usize MAX_NAME = 255;

long resolve_impl(const char* path, Inode* cwd, bool follow_last, Inode** out, int depth, Inode* top);
long (*g_permission)(Inode*, int) = nullptr; // set by the process layer: search (x) permission on directories
Inode* (*g_process_root)() = nullptr;        // set by the process layer: the caller's "/" (chroot), or null

// Where absolute paths of the current process start (chroot / pivot_root).
Inode* top_dir() {
    Inode* r = g_process_root ? g_process_root() : nullptr;
    return r ? r : g_root;
}

// ---- Mount table ----
// A mount covers a directory (any file system) with another file system's
// root. Path lookup crosses from the covered directory into the mounted
// root, and ".." at a mounted root crosses back to the covered directory's
// parent. Mounts can stack; the newest one on a directory is visible.

struct Mount {
    char path[256];
    char source[64];
    char type[16];
    char options[80]; // what /proc/mounts lists ("" = rw,relatime)
    Inode* root;
    Inode* covered; // the directory it sits on
};

constexpr usize MAX_MOUNTS = 256; // containers bring dozens

// A mount namespace's table. Processes share the system's until they
// unshare(CLONE_NEWNS) (or clone with it): then they get a copy, and their
// mounts are theirs alone. One lock for all tables.
struct MountTable {
    Mount mounts[MAX_MOUNTS];
    usize count = 0;
    u32 refs = 1;
    bool pivoted = false; // pivot_root ran in it: the system's root shows only as its put_old mount
};
MountTable g_system_mounts;
void* (*g_process_mounts)() = nullptr; // set by the process layer
IrqSpinlock g_mount_lock;

MountTable* current_table() {
    void* t = g_process_mounts ? g_process_mounts() : nullptr;
    return t ? static_cast<MountTable*>(t) : &g_system_mounts;
}
#define g_mounts (current_table()->mounts)
#define g_mount_count (current_table()->count)

// The file system mounted on `dir`, if any (newest first, following stacks).
Inode* cross_down(Inode* dir) {
    LockGuard guard(g_mount_lock);
    for (usize hops = 0, moved = 1; moved && hops < MAX_MOUNTS; hops++) { // (a cycle can't hang us)
        moved = 0;
        for (usize i = g_mount_count; i-- > 0;) {
            if (g_mounts[i].covered == dir) {
                dir = g_mounts[i].root;
                moved = true;
                break;
            }
        }
    }
    return dir;
}

// If `root` is a mounted file system's root, the directory it covers.
Inode* covered_by(Inode* root) {
    LockGuard guard(g_mount_lock);
    for (usize i = g_mount_count; i-- > 0;)
        if (g_mounts[i].root == root)
            return g_mounts[i].covered;
    return nullptr;
}

// Follow `link` (found in directory `dir`) to its target.
long follow(Inode* link, Inode* dir, Inode** out, int depth) {
    if (depth >= MAX_SYMLINK_DEPTH)
        return -err::LOOP;
    char target[256];
    isize n = link->readlink(target, sizeof(target) - 1);
    if (n < 0)
        return n;
    target[n] = 0;
    return resolve_impl(target, dir, true, out, depth + 1, top_dir());
}

// `top`: the "/" of this walk (absolute paths start there, ".." stops there).
long resolve_impl(const char* path, Inode* cwd, bool follow_last, Inode** out, int depth, Inode* top) {
    if (!path || !*path)
        return -err::NOENT;
    Inode* node = (*path == '/' || !cwd) ? top : cwd;

    const char* p = path;
    while (true) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char* start = p;
        while (*p && *p != '/')
            p++;
        usize len = p - start;
        if (len > MAX_NAME)
            return -err::NAMETOOLONG;
        if (!is_dir(node->mode))
            return -err::NOTDIR;

        bool last = true;
        for (const char* q = p; *q; q++) {
            if (*q != '/') {
                last = false;
                break;
            }
        }
        bool trailing_slash = *p == '/';

        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (node == g_root || node == top)
                continue;
            for (Inode* c; node != g_root && node != top && (c = covered_by(node));)
                node = c; // leave mounted file systems
            if (node != g_root && node != top && node->parent)
                node = node->parent;
            continue;
        }

        if (g_permission)
            if (long r = g_permission(node, 1 /* MAY_EXEC */); r < 0)
                return r;
        Inode* next = node->lookup(start, len);
        if (!next)
            return -err::NOENT;
        next = cross_down(next);
        if (is_lnk(next->mode) && (!last || follow_last || trailing_slash)) {
            long merr = 0;
            Inode* target = nullptr;
            if (next->magic_follow(&merr, &target)) {
                if (merr < 0)
                    return merr;
                next = target;
            } else {
                long r = follow(next, node, &next, depth);
                if (r < 0)
                    return r;
            }
        }
        node = next;
    }
    *out = node;
    return 0;
}

// ---- Bind mounts of directories ----
// Mounts hang on the inode they cover. A bind mount shows the same
// directories at a second place, so a mount made on one of them through
// the bind (bubblewrap's tmpfs on /newroot/dev) would also cover the
// original (/oldroot/dev), and vice versa. Directories seen through a bind
// therefore get inodes of their own: wrappers that pass every operation to
// the real directory (and show its submounts, as MS_REC does). A mount on a
// wrapper covers only that path. Files, devices and links aren't wrapped.
constexpr u8 KIND_BIND = 0xD1;

class BindDir : public Inode {
public:
    Inode* real;
    BindDir* top; // the bind's root (it keeps the wrappers)

    BindDir(Inode* r, BindDir* root_wrapper, Inode* parent_wrapper) : real(r), top(root_wrapper ? root_wrapper : this) {
        anon_kind = KIND_BIND;
        parent = parent_wrapper ? parent_wrapper : this;
        sync();
    }

    void sync() {
        ino = real->ino;
        mode = real->mode;
        nlink = real->nlink;
        uid = real->uid;
        gid = real->gid;
        size = real->size;
        rdev = real->rdev;
        dev = real->dev;
        atime = real->atime;
        mtime = real->mtime;
        ctime = real->ctime;
    }

    // The wrapper for a directory below this one (one per real directory and bind).
    Inode* wrap(Inode* child) {
        if (!is_dir(child->mode))
            return child;
        {
            LockGuard guard(top->m_cache_lock);
            for (Cached* c = top->m_cache; c; c = c->next)
                if (c->real == child) {
                    c->wrapper->sync();
                    return c->wrapper;
                }
        }
        auto* w = new BindDir(child, top, this);
        auto* c = new Cached{child, w, nullptr};
        LockGuard guard(top->m_cache_lock);
        for (Cached* o = top->m_cache; o; o = o->next)
            if (o->real == child) { // made by someone else meanwhile
                delete c;
                delete w;
                return o->wrapper;
            }
        c->next = top->m_cache;
        top->m_cache = c;
        return w;
    }

    Inode* lookup(const char* name, usize len) override {
        Inode* c = real->lookup(name, len);
        if (!c)
            return nullptr;
        return wrap(cross_down(c)); // the real directory's submounts show through (MS_REC)
    }
    long create(const char* name, usize len, u32 new_mode, const char* target, Inode** out) override {
        long r = real->create(name, len, new_mode, target, out);
        if (r == 0 && out && *out)
            *out = wrap(*out);
        sync();
        return r;
    }
    long unlink(const char* name, usize len, bool remove_dir) override {
        long r = real->unlink(name, len, remove_dir);
        sync();
        return r;
    }
    long readdir(u64 index, DirEntry* out) override { return real->readdir(index, out); }
    long rename(const char* old_name, usize old_len, Inode* new_dir, const char* new_name, usize new_len) override {
        long r = real->rename(old_name, old_len, unwrap(new_dir), new_name, new_len);
        sync();
        return r;
    }
    long link(const char* name, usize len, Inode* target) override { return real->link(name, len, unwrap(target)); }
    long statfs(FsStats* out) override { return real->statfs(out); }
    long set_owner(u32 new_mode, u32 new_uid, u32 new_gid) override {
        long r = real->set_owner(new_mode, new_uid, new_gid);
        sync();
        return r;
    }
    long set_times(Timespec access, Timespec modify) override {
        long r = real->set_times(access, modify);
        sync();
        return r;
    }
    long revalidate() override {
        long r = real->revalidate();
        sync();
        return r;
    }
    isize read(u64 offset, void* buf, usize count) override { return real->read(offset, buf, count); }
    bool seekable() const override { return real->seekable(); }
    void on_open(File* f) override { real->on_open(f); }
    void on_release(File* f) override { real->on_release(f); }
    u16 poll(File* f) override { return real->poll(f); }

private:
    struct Cached {
        Inode* real;
        BindDir* wrapper;
        Cached* next;
    };
    Cached* m_cache = nullptr; // (the root's)
    Spinlock m_cache_lock;
};

} // namespace

Inode* unwrap(Inode* n) {
    while (n && n->anon_kind == KIND_BIND)
        n = static_cast<BindDir*>(n)->real;
    return n;
}

Inode* bind_source(Inode* src) {
    if (!is_dir(src->mode))
        return src;
    return new BindDir(unwrap(src), nullptr, nullptr);
}

long Inode::statfs(FsStats* out) {
    // RAM-backed: report system memory, like tmpfs.
    auto st = pmm::stats();
    *out = FsStats{0x858458f6 /* RAMFS_MAGIC */, 4096, st.total_pages, st.free_pages, 1u << 20, 1u << 20};
    return 0;
}

u64 next_fs_dev() {
    static u64 next = 0x10;
    return __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
}

u64 next_ino() { return __atomic_fetch_add(&g_next_ino, 1, __ATOMIC_RELAXED); }

Timespec now() {
    u64 ns = rtc::realtime_ns();
    return Timespec{static_cast<i64>(ns / 1'000'000'000), static_cast<i64>(ns % 1'000'000'000)};
}

void init() {
    g_ram_root = new ramfs::Dir;
    g_ram_root->parent = g_ram_root;
    g_root = g_ram_root;
}

Inode* root() { return g_root; }

long resolve(const char* path, Inode* cwd, bool follow_last, Inode** out) {
    return resolve_impl(path, cwd, follow_last, out, 0, top_dir());
}

long resolve_global(const char* path, Inode** out) { return resolve_impl(path, nullptr, true, out, 0, g_root); }

void set_process_root_hook(Inode* (*fn)()) { g_process_root = fn; }

long resolve_parent(const char* path, Inode* cwd, Inode** parent, const char** name, usize* len) {
    if (!path || !*path)
        return -err::NOENT;

    // Find the last component (ignoring trailing slashes).
    usize total = strlen(path);
    while (total > 1 && path[total - 1] == '/')
        total--;
    usize name_start = total;
    while (name_start > 0 && path[name_start - 1] != '/')
        name_start--;
    usize name_len = total - name_start;
    if (name_len == 0)
        return -err::EXIST; // "/" itself
    // "dir/." and "dir/..": always there (tar's mkdir("./") must not make a
    // child literally named "." -- dpkg's rm -rf of tmp.ci then failed).
    if ((name_len == 1 && path[name_start] == '.') ||
        (name_len == 2 && path[name_start] == '.' && path[name_start + 1] == '.'))
        return -err::EXIST;
    if (name_len > MAX_NAME)
        return -err::NAMETOOLONG;

    Inode* dir;
    if (name_start == 0) {
        dir = (*path == '/' || !cwd) ? top_dir() : cwd;
    } else {
        char buf[1024];
        if (name_start >= sizeof(buf))
            return -err::NAMETOOLONG;
        memcpy(buf, path, name_start);
        buf[name_start] = 0;
        long r = resolve(buf, cwd, true, &dir);
        if (r < 0)
            return r;
    }
    if (!is_dir(dir->mode))
        return -err::NOTDIR;
    *parent = dir;
    *name = path + name_start;
    *len = name_len;
    return 0;
}

// realpath(): an absolute path with "." / ".." removed and every symlink
// resolved (what /proc/self/exe shows on Linux). `base` is the absolute
// directory relative paths start from.
long canonical_path(const char* path, const char* base, char* out, usize size) {
    char work[1024], rest[1024];
    usize n;
    if (path[0] == '/')
        n = static_cast<usize>(snprintf(work, sizeof(work), "%s", path));
    else
        n = static_cast<usize>(snprintf(work, sizeof(work), "%s/%s", base && base[0] ? base : "/", path));
    if (n >= sizeof(work))
        return -err::NAMETOOLONG;
    char result[1024] = "";
    usize rlen = 0;
    const char* p = work;
    for (int links = 0;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char* start = p;
        while (*p && *p != '/')
            p++;
        usize len = static_cast<usize>(p - start);
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (rlen > 0 && result[rlen - 1] != '/')
                rlen--;
            if (rlen > 0)
                rlen--; // drop the slash too
            result[rlen] = 0;
            continue;
        }
        if (rlen + 1 + len >= sizeof(result))
            return -err::NAMETOOLONG;
        result[rlen++] = '/';
        memcpy(result + rlen, start, len);
        rlen += len;
        result[rlen] = 0;
        Inode* node;
        if (long r = resolve(result, nullptr, false, &node); r < 0)
            return r;
        if (is_lnk(node->mode)) {
            if (++links > MAX_SYMLINK_DEPTH)
                return -err::LOOP;
            char target[256];
            isize t = node->readlink(target, sizeof(target) - 1);
            if (t < 0)
                return t;
            target[t] = 0;
            // Continue with target + whatever was left of the path.
            snprintf(rest, sizeof(rest), "%s", p);
            if (target[0] == '/') {
                rlen = 0;
                result[0] = 0;
                snprintf(work, sizeof(work), "%s%s", target, rest);
            } else {
                while (rlen > 0 && result[rlen - 1] != '/') // the link's own name goes
                    rlen--;
                if (rlen > 0)
                    rlen--;
                result[rlen] = 0;
                snprintf(work, sizeof(work), "%s%s", target, rest);
            }
            p = work;
        }
    }
    if (rlen == 0)
        snprintf(out, size, "/");
    else
        snprintf(out, size, "%s", result);
    return 0;
}

long mkdir_p(const char* path, u32 mode) {
    char buf[1024];
    usize n = strlen(path);
    if (n >= sizeof(buf))
        return -err::NAMETOOLONG;
    memcpy(buf, path, n + 1);
    for (usize i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != 0)
            continue;
        char saved = buf[i];
        buf[i] = 0;
        Inode* existing;
        if (resolve(buf, nullptr, true, &existing) < 0) {
            Inode* parent;
            const char* name;
            usize len;
            long r = resolve_parent(buf, nullptr, &parent, &name, &len);
            if (r < 0)
                return r;
            r = parent->create(name, len, S_IFDIR | mode, nullptr, nullptr);
            if (r < 0 && r != -err::EXIST)
                return r;
        } else if (!is_dir(existing->mode)) {
            return -err::NOTDIR;
        }
        buf[i] = saved;
    }
    return 0;
}

long create_file(const char* path, const void* data, usize size, u32 mode) {
    Inode* parent;
    const char* name;
    usize len;
    long r = resolve_parent(path, nullptr, &parent, &name, &len);
    if (r < 0)
        return r;
    Inode* file;
    r = parent->create(name, len, S_IFREG | mode, nullptr, &file);
    if (r < 0)
        return r;
    if (size) {
        isize w = file->write(0, data, size);
        if (w < 0)
            return w;
    }
    return 0;
}

long add_node(const char* path, Inode* node) {
    Inode* parent;
    const char* name;
    usize len;
    long r = resolve_parent(path, nullptr, &parent, &name, &len);
    if (r < 0)
        return r;
    return static_cast<ramfs::Dir*>(parent)->attach(name, len, node);
}

long remove_node(const char* path) {
    Inode* parent;
    const char* name;
    usize len;
    if (long r = resolve_parent(path, nullptr, &parent, &name, &len); r < 0)
        return r;
    return parent->unlink(name, len, false);
}

long Inode::ioctl_file(File*, u64 request, u64 arg) {
    // Linux encodes the argument size in bits 16-29 of most ioctl numbers;
    // older terminal ioctls don't, and use at most a termios (36 bytes).
    // Make sure the buffer is mapped and writable before a driver touches it.
    usize arg_size = (request >> 16) & 0x3FFF;
    if (arg_size == 0)
        arg_size = 36;
    if (arg && uaccess::check(reinterpret_cast<void*>(arg), arg_size, true) < 0)
        return -err::FAULT;
    // Drivers' ioctl() read and write the argument directly: a SMAP window.
    protect::UserAccess window;
    return ioctl(request, arg);
}

File* open_inode(Inode* inode, u32 flags) {
    auto* f = new File;
    f->inode = inode;
    f->flags = flags;
    inode->on_open(f);
    return f;
}

void file_set_path(File* f, const char* path) {
    kfree(f->path);
    usize n = strlen(path);
    f->path = static_cast<char*>(kmalloc(n + 1));
    if (f->path)
        memcpy(f->path, path, n + 1);
}

void file_get(File* f) { __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED); }

void (*file_release_hook)(File* f) = nullptr;

void file_put(File* f) {
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (file_release_hook)
            file_release_hook(f);
        if (inotify::active() && (is_reg(f->inode->mode) || is_dir(f->inode->mode)))
            inotify::file_event(f, (f->flags & O_ACCMODE) != O_RDONLY ? inotify::IN_CLOSE_WRITE : inotify::IN_CLOSE_NOWRITE);
        f->inode->on_release(f);
        kfree(f->path);
        delete f;
    }
}

// ---- Mounts ----

long mount_at(const char* path, Inode* fs_root, const char* source, const char* type, const char* options) {
    Inode* target;
    if (long r = resolve(path, nullptr, true, &target); r < 0)
        return r;
    // Directories on directories; a file only on a file (bind mounts of
    // single files and device nodes, as containers do).
    if (is_dir(target->mode) != is_dir(fs_root->mode))
        return is_dir(target->mode) ? -err::NOTDIR : -err::ISDIR;
    if (target == fs_root)
        return 0; // already what's there (a bind of a directory onto itself)
    LockGuard guard(g_mount_lock);
    if (g_mount_count >= MAX_MOUNTS)
        return -err::NOMEM;
    Mount& m = g_mounts[g_mount_count++];
    snprintf(m.path, sizeof(m.path), "%s", path);
    snprintf(m.source, sizeof(m.source), "%s", source);
    snprintf(m.type, sizeof(m.type), "%s", type);
    snprintf(m.options, sizeof(m.options), "%s", options ? options : "");
    m.root = fs_root;
    m.covered = target;
    if (!fs_root->parent)
        fs_root->parent = fs_root;
    return 0;
}

// The mount `path` names, as the caller sees it: the newest mount whose root
// `node` (what path resolves to) is, or that covers it, preferring one
// recorded under that path. Mount lock held.
long find_mount(const char* path, Inode* node, usize* index) {
    long best = -1;
    for (usize i = g_mount_count; i-- > 0;) {
        if (g_mounts[i].root != node && g_mounts[i].covered != node)
            continue;
        if (strcmp(g_mounts[i].path, path) == 0) {
            best = static_cast<long>(i);
            break;
        }
        if (best < 0)
            best = static_cast<long>(i);
    }
    if (best < 0)
        return node == top_dir() ? 1 : -err::INVAL; // pivot_root(".", ".") left nothing to detach
    *index = static_cast<usize>(best);
    return 0;
}

// MS_MOVE: the mount at `from` now sits on `to`.
long move_mount(const char* from, const char* to) {
    Inode *target, *node;
    if (long r = resolve(to, nullptr, true, &target); r < 0)
        return r;
    if (long r = resolve(from, nullptr, true, &node); r < 0)
        return r;
    LockGuard guard(g_mount_lock);
    usize i;
    if (long r = find_mount(from, node, &i); r != 0)
        return r < 0 ? r : -err::INVAL;
    g_mounts[i].covered = target;
    snprintf(g_mounts[i].path, sizeof(g_mounts[i].path), "%s", to);
    return 0;
}

// The source (device) recorded for the mount `path` names; "" if none.
void mount_source(const char* path, char* out, usize size) {
    out[0] = 0;
    LockGuard guard(g_mount_lock);
    for (usize i = g_mount_count; i-- > 0;)
        if (strcmp(g_mounts[i].path, path) == 0) {
            snprintf(out, size, "%s", g_mounts[i].source);
            return;
        }
}

long unmount(const char* path) {
    Inode* node;
    if (long r = resolve(path, nullptr, true, &node); r < 0)
        return r;
    LockGuard guard(g_mount_lock);
    usize found;
    if (long r = find_mount(path, node, &found); r != 0)
        return r < 0 ? r : 0;
    for (usize i = found + 1; i-- > found;) {
        // Something mounted inside it? (Bind mounts share their file
        // system with others; containers detach them freely.)
        bool shared = strcmp(g_mounts[i].type, "bind") == 0 || strcmp(g_mounts[i].type, "rootfs") == 0;
        for (usize j = 0; j < g_mount_count && !shared; j++)
            if (j != i && g_mounts[j].covered->dev == g_mounts[i].root->dev && g_mounts[i].root->dev != 1)
                return -err::BUSY;
        for (usize j = i; j + 1 < g_mount_count; j++)
            g_mounts[j] = g_mounts[j + 1];
        g_mount_count--;
        return 0;
    }
    return -err::INVAL;
}

bool is_mountpoint(Inode* dir) {
    LockGuard guard(g_mount_lock);
    for (usize i = 0; i < g_mount_count; i++)
        if (g_mounts[i].covered == dir || g_mounts[i].root == dir)
            return true;
    return dir == g_root;
}

void set_root(Inode* new_root, const char* source, const char* type) {
    new_root->parent = new_root;
    g_root = new_root;
    snprintf(g_root_source, sizeof(g_root_source), "%s", source);
    snprintf(g_root_type, sizeof(g_root_type), "%s", type);
}

Inode* ram_root() { return g_ram_root; }

void set_permission_hook(long (*fn)(Inode*, int)) { g_permission = fn; }

// pivot_root in a private mount namespace: every mount point is renamed to
// how it looks from the new root (Linux's mountinfo is relative to it, and
// bubblewrap finds its mounts there): mounts under `new_root` lose that
// prefix, the others move under `put_old` (as seen from the new root).
void pivot_mounts(const char* new_root, const char* put_old) {
    LockGuard guard(g_mount_lock);
    MountTable* t = current_table();
    if (t == &g_system_mounts)
        return; // the system's table keeps the system's view
    usize n = strlen(new_root);
    bool top = n == 1;
    for (usize i = 0; i < t->count; i++) {
        Mount& m = t->mounts[i];
        char out[256];
        if (top || (strncmp(m.path, new_root, n) == 0 && (m.path[n] == '/' || !m.path[n])))
            snprintf(out, sizeof(out), "%s", top ? m.path : (m.path[n] ? m.path + n : "/"));
        else
            snprintf(out, sizeof(out), "%s%s", strcmp(put_old, "/") == 0 ? "" : put_old,
                     strcmp(m.path, "/") == 0 ? "" : m.path);
        snprintf(m.path, sizeof(m.path), "%s", out[0] ? out : "/");
    }
    t->pivoted = true;
}

void for_each_mount(void (*fn)(const char*, const char*, const char*, const char*, void*), void* ctx) {
    if (!current_table()->pivoted)
        fn(g_root_source, "/", g_root_type, "", ctx); // not "rootfs": tools like df skip entries by that name
    LockGuard guard(g_mount_lock);
    for (usize i = 0; i < g_mount_count; i++)
        fn(g_mounts[i].source, g_mounts[i].path, g_mounts[i].type, g_mounts[i].options, ctx);
}

} // namespace vfs

namespace vfs {

void set_process_mounts_hook(void* (*fn)()) { g_process_mounts = fn; }

// A new mount namespace: a copy of `from`'s table (nullptr: the system's).
void* mount_ns_copy(void* from) {
    auto* t = new MountTable;
    LockGuard guard(g_mount_lock);
    MountTable* src = from ? static_cast<MountTable*>(from) : &g_system_mounts;
    for (usize i = 0; i < src->count; i++)
        t->mounts[i] = src->mounts[i];
    t->count = src->count;
    t->pivoted = src->pivoted;
    return t;
}

void mount_ns_get(void* ns) {
    if (ns)
        __atomic_add_fetch(&static_cast<MountTable*>(ns)->refs, 1, __ATOMIC_RELAXED);
}

void mount_ns_put(void* ns) {
    if (ns && __atomic_sub_fetch(&static_cast<MountTable*>(ns)->refs, 1, __ATOMIC_ACQ_REL) == 0)
        delete static_cast<MountTable*>(ns);
}

} // namespace vfs
