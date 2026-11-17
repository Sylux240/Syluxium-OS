#include <fs/ramfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace vfs {

namespace {

ramfs::Dir* g_root = nullptr;
u64 g_next_ino = 1;
constexpr int MAX_SYMLINK_DEPTH = 40;
constexpr usize MAX_NAME = 255;

long resolve_impl(const char* path, Inode* cwd, bool follow_last, Inode** out, int depth);

// Follow `link` (found in directory `dir`) to its target.
long follow(Inode* link, Inode* dir, Inode** out, int depth) {
    if (depth >= MAX_SYMLINK_DEPTH)
        return -err::LOOP;
    char target[256];
    isize n = link->readlink(target, sizeof(target) - 1);
    if (n < 0)
        return n;
    target[n] = 0;
    return resolve_impl(target, dir, true, out, depth + 1);
}

long resolve_impl(const char* path, Inode* cwd, bool follow_last, Inode** out, int depth) {
    if (!path || !*path)
        return -err::NOENT;
    Inode* node = (*path == '/' || !cwd) ? g_root : cwd;

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
            if (node->parent)
                node = node->parent;
            continue;
        }

        Inode* next = node->lookup(start, len);
        if (!next)
            return -err::NOENT;
        if (is_lnk(next->mode) && (!last || follow_last || trailing_slash)) {
            long r = follow(next, node, &next, depth);
            if (r < 0)
                return r;
        }
        node = next;
    }
    *out = node;
    return 0;
}

} // namespace

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
    g_root = new ramfs::Dir;
    g_root->parent = g_root;
}

Inode* root() { return g_root; }

long resolve(const char* path, Inode* cwd, bool follow_last, Inode** out) {
    return resolve_impl(path, cwd, follow_last, out, 0);
}

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
    if (name_len > MAX_NAME)
        return -err::NAMETOOLONG;

    Inode* dir;
    if (name_start == 0) {
        dir = (*path == '/') ? g_root : (cwd ? cwd : g_root);
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

void file_put(File* f) {
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        f->inode->on_release(f);
        kfree(f->path);
        delete f;
    }
}

// ---- Mounts ----

namespace {

struct Mount {
    char path[256];
    char source[64];
    char type[16];
    Inode* root;
    Inode* covered; // the directory that was there before
};

constexpr usize MAX_MOUNTS = 32;
Mount g_mounts[MAX_MOUNTS];
usize g_mount_count = 0;
IrqSpinlock g_mount_lock;

} // namespace

long mount_at(const char* path, Inode* fs_root, const char* source, const char* type) {
    Inode* parent;
    const char* name;
    usize len;
    if (long r = resolve_parent(path, nullptr, &parent, &name, &len); r < 0)
        return r;
    Inode* target = parent->lookup(name, len);
    if (!target)
        return -err::NOENT;
    if (!is_dir(target->mode))
        return -err::NOTDIR;
    LockGuard guard(g_mount_lock);
    if (g_mount_count >= MAX_MOUNTS)
        return -err::NOMEM;
    // Only directories inside the RAM root file system can be covered for now.
    auto* dir = static_cast<ramfs::Dir*>(parent);
    if (!dir->replace(name, len, fs_root))
        return -err::INVAL;
    Mount& m = g_mounts[g_mount_count++];
    snprintf(m.path, sizeof(m.path), "%s", path);
    snprintf(m.source, sizeof(m.source), "%s", source);
    snprintf(m.type, sizeof(m.type), "%s", type);
    m.root = fs_root;
    m.covered = target;
    return 0;
}

long unmount(const char* path) {
    LockGuard guard(g_mount_lock);
    for (usize i = g_mount_count; i-- > 0;) {
        if (strcmp(g_mounts[i].path, path) != 0)
            continue;
        Inode* parent;
        const char* name;
        usize len;
        if (long r = resolve_parent(path, nullptr, &parent, &name, &len); r < 0)
            return r;
        static_cast<ramfs::Dir*>(parent)->replace(name, len, g_mounts[i].covered);
        g_mounts[i] = g_mounts[--g_mount_count];
        return 0;
    }
    return -err::INVAL;
}

void for_each_mount(void (*fn)(const char*, const char*, const char*, void*), void* ctx) {
    fn("ramfs", "/", "ramfs", ctx); // not "rootfs": tools like df skip entries by that name
    LockGuard guard(g_mount_lock);
    for (usize i = 0; i < g_mount_count; i++)
        fn(g_mounts[i].source, g_mounts[i].path, g_mounts[i].type, ctx);
}

} // namespace vfs
