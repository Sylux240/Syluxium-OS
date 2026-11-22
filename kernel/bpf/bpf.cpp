// eBPF objects without execution (see bpf.hpp): bpf(2), maps, programs,
// attachments, IDs and the BPF file system.
#include <bpf/bpf.hpp>

#include <arch/x86_64/percpu.hpp>
#include <fs/ramfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>
#include <syscall/syscall.hpp>

namespace bpf {
namespace {

// ---- Linux's numbers ----
enum Cmd : int {
    MAP_CREATE = 0, MAP_LOOKUP_ELEM = 1, MAP_UPDATE_ELEM = 2, MAP_DELETE_ELEM = 3, MAP_GET_NEXT_KEY = 4,
    PROG_LOAD = 5, OBJ_PIN = 6, OBJ_GET = 7, PROG_ATTACH = 8, PROG_DETACH = 9, PROG_TEST_RUN = 10,
    PROG_GET_NEXT_ID = 11, MAP_GET_NEXT_ID = 12, PROG_GET_FD_BY_ID = 13, MAP_GET_FD_BY_ID = 14,
    OBJ_GET_INFO_BY_FD = 15, PROG_QUERY = 16, RAW_TRACEPOINT_OPEN = 17, BTF_LOAD = 18, BTF_GET_FD_BY_ID = 19,
    TASK_FD_QUERY = 20, MAP_LOOKUP_AND_DELETE_ELEM = 21, MAP_FREEZE = 22, BTF_GET_NEXT_ID = 23,
    LINK_CREATE = 28, LINK_UPDATE = 29, LINK_GET_FD_BY_ID = 30, LINK_GET_NEXT_ID = 31, LINK_DETACH = 34,
};
enum MapType : u32 {
    HASH = 1, ARRAY = 2, PROG_ARRAY = 3, PERF_EVENT_ARRAY = 4, PERCPU_HASH = 5, PERCPU_ARRAY = 6,
    STACK_TRACE = 7, CGROUP_ARRAY = 8, LRU_HASH = 9, LRU_PERCPU_HASH = 10, LPM_TRIE = 11, ARRAY_OF_MAPS = 12,
    HASH_OF_MAPS = 13, DEVMAP = 14, SOCKMAP = 15, CPUMAP = 16, XSKMAP = 17, SOCKHASH = 18,
    CGROUP_STORAGE = 19, REUSEPORT_SOCKARRAY = 20, PERCPU_CGROUP_STORAGE = 21, QUEUE = 22, STACK = 23,
    SK_STORAGE = 24, DEVMAP_HASH = 25, STRUCT_OPS = 26, RINGBUF = 27, INODE_STORAGE = 28, TASK_STORAGE = 29,
    BLOOM_FILTER = 30, USER_RINGBUF = 31, CGRP_STORAGE = 32, MAP_TYPE_MAX = 40,
};
constexpr u64 BPF_ANY = 0, BPF_NOEXIST = 1, BPF_EXIST = 2, BPF_F_LOCK = 4;
constexpr u32 BPF_F_RDONLY = 1 << 3, BPF_F_WRONLY = 1 << 4;
constexpr long ENOTSUPP = 524; // Linux's internal "not supported" (bpf returns it for test runs)

constexpr u8 KIND_BPF = 0xBF; // vfs::Inode::anon_kind of every bpf object
constexpr u32 BPF_FS_MAGIC = 0xcafe4a11;
constexpr usize ATTR_SIZE = 160; // union bpf_attr as of Linux 6.12 is a bit smaller; the rest must be 0

u32 ncpus() { return percpu::online_count() ? percpu::online_count() : 1; }
u32 round8(u32 n) { return (n + 7) & ~7u; }

// ---- objects ----
enum class Kind : u8 { Map, Prog, Btf, Link };

class Object;
Spinlock g_ids_lock;
Object* g_objects = nullptr; // all live objects (for IDs)
u32 g_next_id[4] = {1, 1, 1, 1};

// Lifetime: while an open file or a pin (a name in bpffs, the inode's
// nlink) refers to it.
class Object : public vfs::Inode {
public:
    explicit Object(Kind k) : kind(k) {
        ino = vfs::next_ino();
        mode = vfs::S_IFREG | 0600;
        uid = proc::cred().fsuid;
        gid = proc::cred().fsgid;
        nlink = 0;
        anon_kind = KIND_BPF;
        atime = mtime = ctime = vfs::now();
        LockGuard g(g_ids_lock);
        id = g_next_id[static_cast<int>(k)]++;
        next = g_objects;
        g_objects = this;
    }
    ~Object() override {
        LockGuard g(g_ids_lock);
        for (Object** p = &g_objects; *p; p = &(*p)->next) {
            if (*p == this) {
                *p = next;
                break;
            }
        }
    }
    bool seekable() const override { return false; }
    isize read(u64, void*, usize) override { return -err::INVAL; }
    isize write(u64, const void*, usize) override { return -err::INVAL; }
    void on_open(vfs::File*) override { __atomic_add_fetch(&m_opens, 1, __ATOMIC_ACQ_REL); }
    void on_release(vfs::File*) override {
        if (__atomic_sub_fetch(&m_opens, 1, __ATOMIC_ACQ_REL) == 0 && nlink == 0)
            delete this;
    }
    void unlinked() override {
        if (__atomic_load_n(&m_opens, __ATOMIC_ACQUIRE) == 0)
            delete this;
    }

    Kind kind;
    u32 id;
    char name[16] = {};
    Object* next = nullptr;

private:
    u32 m_opens = 0;
};

// One entry of a hash-type map, on the insertion-ordered list (iteration,
// LRU eviction) and on its bucket's chain.
struct Entry {
    Entry* prev;
    Entry* next;
    Entry* chain;
    u8* key;
    u8* value;
};

class Map : public Object {
public:
    Map() : Object(Kind::Map) {}
    ~Map() override {
        while (m_head)
            remove(m_head);
        kfree(m_buckets);
        kfree(m_array);
    }

    u32 type = 0, key_size = 0, value_size = 0, max_entries = 0, flags = 0;
    bool frozen = false;

    bool is_array() const {
        switch (type) {
        case ARRAY: case PERCPU_ARRAY: case PROG_ARRAY: case PERF_EVENT_ARRAY: case CGROUP_ARRAY:
        case ARRAY_OF_MAPS: case DEVMAP: case CPUMAP: case XSKMAP: case REUSEPORT_SOCKARRAY:
            return true;
        default:
            return false;
        }
    }
    bool is_percpu() const {
        return type == PERCPU_HASH || type == PERCPU_ARRAY || type == LRU_PERCPU_HASH || type == PERCPU_CGROUP_STORAGE;
    }
    bool is_lru() const { return type == LRU_HASH || type == LRU_PERCPU_HASH; }
    // Bytes of a value as user space passes it (per-CPU: one 8-aligned slot per CPU).
    u32 user_value_size() const { return is_percpu() ? round8(value_size) * ncpus() : value_size; }

    long setup() {
        if (is_array()) {
            if (key_size != 4)
                return -err::INVAL;
            usize bytes = static_cast<usize>(max_entries) * user_value_size();
            if (bytes > (256ull << 20))
                return -err::NOMEM;
            m_array = static_cast<u8*>(kzalloc(bytes ? bytes : 1));
            return m_array ? 0 : -err::NOMEM;
        }
        m_nbuckets = 16;
        while (m_nbuckets < max_entries && m_nbuckets < 65536)
            m_nbuckets <<= 1;
        m_buckets = static_cast<Entry**>(kzalloc(m_nbuckets * sizeof(Entry*)));
        return m_buckets ? 0 : -err::NOMEM;
    }

    // All of these run with `lock` held; buffers are kernel memory.
    long lookup(const u8* key, u8* out) {
        if (is_array()) {
            u32 i = *reinterpret_cast<const u32*>(key);
            if (i >= max_entries)
                return -err::NOENT;
            memcpy(out, m_array + static_cast<usize>(i) * user_value_size(), user_value_size());
            return 0;
        }
        Entry* e = type == LPM_TRIE ? longest_prefix(key) : find(key);
        if (!e)
            return -err::NOENT;
        memcpy(out, e->value, user_value_size());
        return 0;
    }

    long update(const u8* key, const u8* value, u64 how) {
        if (is_array()) {
            u32 i = *reinterpret_cast<const u32*>(key);
            if (i >= max_entries)
                return -err::TOOBIG;
            if (how == BPF_NOEXIST)
                return -err::EXIST; // array elements always exist
            memcpy(m_array + static_cast<usize>(i) * user_value_size(), value, user_value_size());
            return 0;
        }
        if (type == LPM_TRIE) {
            u32 prefix = *reinterpret_cast<const u32*>(key);
            if (prefix > (key_size - 4) * 8)
                return -err::INVAL;
        }
        Entry* e = find(key);
        if (e) {
            if (how == BPF_NOEXIST)
                return -err::EXIST;
            memcpy(e->value, value, user_value_size());
            // most recently used goes to the back
            unlink_order(e);
            link_order(e);
            return 0;
        }
        if (how == BPF_EXIST)
            return -err::NOENT;
        if (m_count >= max_entries) {
            if (!is_lru())
                return -err::TOOBIG;
            remove(m_head); // least recently used
        }
        e = static_cast<Entry*>(kmalloc(sizeof(Entry) + key_size + user_value_size()));
        if (!e)
            return -err::NOMEM;
        e->key = reinterpret_cast<u8*>(e + 1);
        e->value = e->key + key_size;
        memcpy(e->key, key, key_size);
        memcpy(e->value, value, user_value_size());
        u32 b = bucket(key);
        e->chain = m_buckets[b];
        m_buckets[b] = e;
        link_order(e);
        m_count++;
        return 0;
    }

    long erase(const u8* key) {
        if (is_array())
            return -err::INVAL;
        Entry* e = find(key);
        if (!e)
            return -err::NOENT;
        remove(e);
        return 0;
    }

    // `key` null (or not in the map): the first key.
    long next_key(const u8* key, u8* out) {
        if (is_array()) {
            u32 i = key ? *reinterpret_cast<const u32*>(key) : ~0u;
            u32 n = (i >= max_entries) ? 0 : i + 1;
            if (n >= max_entries)
                return -err::NOENT;
            memcpy(out, &n, 4);
            return 0;
        }
        Entry* e = key ? find(key) : nullptr;
        Entry* n = e ? e->next : m_head;
        if (!n)
            return -err::NOENT;
        memcpy(out, n->key, key_size);
        return 0;
    }

    sched::Mutex lock;

private:
    u32 bucket(const u8* key) const {
        u32 h = 2166136261u; // FNV-1a
        for (u32 i = 0; i < key_size; i++)
            h = (h ^ key[i]) * 16777619u;
        return h & (m_nbuckets - 1);
    }
    Entry* find(const u8* key) const {
        for (Entry* e = m_buckets[bucket(key)]; e; e = e->chain)
            if (memcmp(e->key, key, key_size) == 0)
                return e;
        return nullptr;
    }
    // LPM trie keys: u32 prefix length (bits), then the data.
    Entry* longest_prefix(const u8* key) const {
        u32 want = *reinterpret_cast<const u32*>(key);
        const u8* data = key + 4;
        Entry* best = nullptr;
        u32 best_len = 0;
        for (Entry* e = m_head; e; e = e->next) {
            u32 len = *reinterpret_cast<const u32*>(e->key);
            if (len > want || (best && len < best_len))
                continue;
            const u8* ed = e->key + 4;
            u32 full = len / 8, rest = len % 8;
            if (memcmp(ed, data, full) != 0)
                continue;
            if (rest) {
                u8 mask = static_cast<u8>(0xFF << (8 - rest));
                if ((ed[full] & mask) != (data[full] & mask))
                    continue;
            }
            best = e;
            best_len = len;
        }
        return best;
    }
    void link_order(Entry* e) {
        e->next = nullptr;
        e->prev = m_tail;
        if (m_tail)
            m_tail->next = e;
        else
            m_head = e;
        m_tail = e;
    }
    void unlink_order(Entry* e) {
        if (e->prev)
            e->prev->next = e->next;
        else
            m_head = e->next;
        if (e->next)
            e->next->prev = e->prev;
        else
            m_tail = e->prev;
    }
    void remove(Entry* e) {
        unlink_order(e);
        for (Entry** p = &m_buckets[bucket(e->key)]; *p; p = &(*p)->chain) {
            if (*p == e) {
                *p = e->chain;
                break;
            }
        }
        m_count--;
        kfree(e);
    }

    Entry** m_buckets = nullptr;
    u32 m_nbuckets = 0;
    Entry* m_head = nullptr;
    Entry* m_tail = nullptr;
    u32 m_count = 0;
    u8* m_array = nullptr;
};

class Prog : public Object {
public:
    Prog() : Object(Kind::Prog) {}
    u32 type = 0, insn_cnt = 0, expected_attach_type = 0;
    u64 load_time = 0;
    u32 created_by_uid = 0;
};

class Blob : public Object { // BTF data and links: kept, not interpreted
public:
    explicit Blob(Kind k) : Object(k) {}
    u32 size = 0;
    u32 prog_id = 0, attach_type = 0;
};

// cgroup attachments (recorded for PROG_QUERY; programs never run).
struct Attachment {
    vfs::Inode* target;
    u32 attach_type;
    u32 prog_id;
    Attachment* next;
};
Spinlock g_attach_lock;
Attachment* g_attachments = nullptr;

// ---- helpers ----
Object* object_of(vfs::File* f) {
    return f && f->inode && f->inode->anon_kind == KIND_BPF ? static_cast<Object*>(f->inode) : nullptr;
}

// A new file for `obj` in the caller's table (bpf fds are always close-on-exec).
long install(Object* obj, u32 open_flags = 0) {
    u32 acc = (open_flags & BPF_F_RDONLY) ? vfs::O_RDONLY : (open_flags & BPF_F_WRONLY) ? vfs::O_WRONLY : vfs::O_RDWR;
    vfs::File* f = vfs::open_inode(obj, acc);
    if (!f)
        return -err::NOMEM;
    const char* what = obj->kind == Kind::Map ? "bpf-map" : obj->kind == Kind::Prog ? "bpf-prog"
                       : obj->kind == Kind::Btf ? "btf" : "bpf_link";
    char path[32];
    snprintf(path, sizeof(path), "anon_inode:%s", what);
    vfs::file_set_path(f, path);
    long fd = proc::fd_install(proc::current(), f, 0, true);
    if (fd < 0)
        vfs::file_put(f);
    return fd;
}

// The object behind `fd`, with a reference on its file (file_put when done).
struct Held {
    vfs::File* file = nullptr;
    Object* obj = nullptr;
    ~Held() {
        if (file)
            vfs::file_put(file);
    }
};
long hold(int fd, Held* h, Kind want) {
    h->file = proc::fd_get(proc::current(), fd);
    if (!h->file)
        return -err::BADF;
    h->obj = object_of(h->file);
    if (!h->obj || h->obj->kind != want)
        return -err::INVAL;
    return 0;
}

Object* find_id(Kind k, u32 id) {
    for (Object* o = g_objects; o; o = o->next)
        if (o->kind == k && o->id == id)
            return o;
    return nullptr;
}

void copy_name(char* dst, const u8* src) {
    memcpy(dst, src, 16);
    dst[15] = 0;
}

template <typename T>
T get(const u8* attr, usize off) {
    T v;
    memcpy(&v, attr + off, sizeof(T));
    return v;
}
template <typename T>
void put(u8* attr, usize off, T v) {
    memcpy(attr + off, &v, sizeof(T));
}

// Absolute or cwd-relative path -> parent directory inode and last component.
long split_path(const char* path, vfs::Inode** dir, char* name, usize name_max) {
    const char* slash = nullptr;
    for (const char* q = path; *q; q++)
        if (*q == '/')
            slash = q;
    const char* last = slash ? slash + 1 : path;
    usize n = strlen(last);
    if (n == 0 || n >= name_max || strcmp(last, ".") == 0 || strcmp(last, "..") == 0)
        return -err::INVAL;
    memcpy(name, last, n + 1);
    proc::Process* p = proc::current();
    if (!slash)
        *dir = p->cwd;
    else {
        char parent[512];
        usize plen = static_cast<usize>(slash - path);
        if (plen >= sizeof(parent))
            return -err::NAMETOOLONG;
        memcpy(parent, path, plen);
        parent[plen] = 0;
        if (plen == 0)
            snprintf(parent, sizeof(parent), "/");
        if (long r = vfs::resolve(parent, p->cwd, true, dir); r < 0)
            return r;
    }
    return vfs::is_dir((*dir)->mode) ? 0 : -err::NOTDIR;
}

// ---- commands ----
long map_create(const u8* a) {
    if (!proc::capable())
        return -err::PERM;
    auto type = get<u32>(a, 0);
    auto ks = get<u32>(a, 4), vs = get<u32>(a, 8), max = get<u32>(a, 12), flags = get<u32>(a, 16);
    if (type == 0 || type >= MAP_TYPE_MAX)
        return -err::INVAL;
    bool keyless = type == RINGBUF || type == USER_RINGBUF || type == QUEUE || type == STACK ||
                   type == BLOOM_FILTER;
    if (!keyless && (ks == 0 || vs == 0))
        return -err::INVAL;
    if (max == 0 && type != SK_STORAGE && type != INODE_STORAGE && type != TASK_STORAGE &&
        type != CGRP_STORAGE && type != CGROUP_STORAGE && type != PERCPU_CGROUP_STORAGE)
        return -err::INVAL;
    if (ks > 512 || vs > (1u << 20))
        return -err::TOOBIG;
    if (type == LPM_TRIE && ks <= 4)
        return -err::INVAL;
    auto* m = new Map;
    m->type = type;
    m->key_size = ks;
    m->value_size = vs;
    m->max_entries = max ? max : 1;
    m->flags = flags;
    // Linux: device maps are read-only for programs, and say so in their flags
    // (Android's bpfloader compares the flags it reads back).
    if (type == DEVMAP || type == DEVMAP_HASH)
        m->flags |= 0x80; // BPF_F_RDONLY_PROG
    copy_name(m->name, a + 28);
    if (!keyless) {
        if (long r = m->setup(); r < 0) {
            delete m;
            return r;
        }
    }
    long fd = install(m);
    if (fd < 0)
        delete m;
    return fd;
}

long map_elem(int cmd, const u8* a) {
    Held h;
    if (long r = hold(static_cast<int>(get<u32>(a, 0)), &h, Kind::Map); r < 0)
        return r;
    auto* m = static_cast<Map*>(h.obj);
    if (m->type == RINGBUF || m->type == USER_RINGBUF || m->type == BLOOM_FILTER || m->key_size == 0)
        return -err::INVAL;
    u64 ukey = get<u64>(a, 8), uval = get<u64>(a, 16), flags = get<u64>(a, 24);
    u32 vsz = m->user_value_size();
    u8* key = static_cast<u8*>(kmalloc(m->key_size));
    u8* val = static_cast<u8*>(kzalloc(vsz ? vsz : 1));
    long r = 0;
    if (!key || !val) {
        r = -err::NOMEM;
    } else if (cmd == MAP_GET_NEXT_KEY) {
        bool have = ukey != 0;
        if (have)
            r = uaccess::copy_from(key, reinterpret_cast<const void*>(ukey), m->key_size);
        if (r >= 0) {
            u8* next = static_cast<u8*>(kmalloc(m->key_size));
            if (!next) {
                r = -err::NOMEM;
            } else {
                {
                    LockGuard g(m->lock);
                    r = m->next_key(have ? key : nullptr, next);
                }
                if (r >= 0)
                    r = uaccess::copy_to(reinterpret_cast<void*>(uval), next, m->key_size);
                kfree(next);
            }
        }
    } else if ((r = uaccess::copy_from(key, reinterpret_cast<const void*>(ukey), m->key_size)) >= 0) {
        switch (cmd) {
        case MAP_LOOKUP_ELEM:
        case MAP_LOOKUP_AND_DELETE_ELEM:
            if (flags & ~BPF_F_LOCK) {
                r = -err::INVAL;
                break;
            }
            {
                LockGuard g(m->lock);
                r = m->lookup(key, val);
                if (r >= 0 && cmd == MAP_LOOKUP_AND_DELETE_ELEM)
                    r = m->is_array() ? -ENOTSUPP : m->erase(key);
            }
            if (r >= 0)
                r = uaccess::copy_to(reinterpret_cast<void*>(uval), val, vsz);
            break;
        case MAP_UPDATE_ELEM:
            if ((flags & ~BPF_F_LOCK) > BPF_EXIST) {
                r = -err::INVAL;
                break;
            }
            if (m->frozen) {
                r = -err::PERM;
                break;
            }
            if ((r = uaccess::copy_from(val, reinterpret_cast<const void*>(uval), vsz)) < 0)
                break;
            {
                LockGuard g(m->lock);
                r = m->update(key, val, flags & ~BPF_F_LOCK);
            }
            break;
        case MAP_DELETE_ELEM:
            if (m->frozen) {
                r = -err::PERM;
                break;
            }
            {
                LockGuard g(m->lock);
                r = m->erase(key);
            }
            break;
        }
    }
    kfree(key);
    kfree(val);
    return r < 0 ? r : 0;
}

long prog_load(const u8* a) {
    if (!proc::capable())
        return -err::PERM;
    auto type = get<u32>(a, 0), cnt = get<u32>(a, 4);
    u64 insns = get<u64>(a, 8), license = get<u64>(a, 16);
    u32 log_level = get<u32>(a, 24), log_size = get<u32>(a, 28);
    u64 log_buf = get<u64>(a, 32);
    if (cnt == 0 || cnt > 1000000 || !insns)
        return -err::INVAL;
    // The instructions must be readable (a bad pointer fails like on Linux).
    u8 probe[8];
    if (long r = uaccess::copy_from(probe, reinterpret_cast<const void*>(insns), sizeof(probe)); r < 0)
        return r;
    if (license) {
        char lic[128];
        if (long r = uaccess::string_from(reinterpret_cast<const char*>(license), lic, sizeof(lic)); r < 0)
            return r;
    }
    if (log_level && log_buf && log_size) {
        char empty = 0; // a verifier log with nothing to say
        uaccess::copy_to(reinterpret_cast<void*>(log_buf), &empty, 1);
    }
    auto* p = new Prog;
    p->type = type;
    p->insn_cnt = cnt;
    copy_name(p->name, a + 48);
    p->expected_attach_type = get<u32>(a, 68);
    p->created_by_uid = proc::cred().ruid;
    long fd = install(p);
    if (fd < 0)
        delete p;
    return fd;
}

long obj_pin(const u8* a) {
    u64 upath = get<u64>(a, 0);
    Held h;
    h.file = proc::fd_get(proc::current(), static_cast<int>(get<u32>(a, 8)));
    if (!h.file)
        return -err::BADF;
    h.obj = object_of(h.file);
    if (!h.obj)
        return -err::INVAL;
    char path[512], name[256];
    if (long r = uaccess::string_from(reinterpret_cast<const char*>(upath), path, sizeof(path)); r < 0)
        return r;
    vfs::Inode* dir;
    if (long r = split_path(path, &dir, name, sizeof(name)); r < 0)
        return r;
    if (!dir->is_ramfs_dir())
        return -err::PERM; // only in the BPF file system (and other RAM directories)
    if (long r = proc::permission(dir, proc::MAY_WRITE | proc::MAY_EXEC); r < 0)
        return r;
    if (dir->lookup(name, strlen(name)))
        return -err::EXIST;
    long r = static_cast<ramfs::Dir*>(dir)->attach(name, strlen(name), h.obj);
    if (r == 0)
        h.obj->nlink++;
    return r;
}

long obj_get(const u8* a) {
    char path[512];
    if (long r = uaccess::string_from(reinterpret_cast<const char*>(get<u64>(a, 0)), path, sizeof(path)); r < 0)
        return r;
    u32 flags = get<u32>(a, 12);
    if (flags & ~(BPF_F_RDONLY | BPF_F_WRONLY) || (flags & BPF_F_RDONLY && flags & BPF_F_WRONLY))
        return -err::INVAL;
    vfs::Inode* inode;
    if (long r = vfs::resolve(path, proc::current()->cwd, true, &inode); r < 0)
        return r;
    if (inode->anon_kind != KIND_BPF)
        return -err::INVAL;
    int want = (flags & BPF_F_RDONLY) ? proc::MAY_READ : (flags & BPF_F_WRONLY) ? proc::MAY_WRITE
                                                                                 : proc::MAY_READ | proc::MAY_WRITE;
    if (long r = proc::permission(inode, want); r < 0)
        return r;
    return install(static_cast<Object*>(inode), flags);
}

long prog_attach(int cmd, const u8* a) {
    if (!proc::capable())
        return -err::PERM;
    u32 target_fd = get<u32>(a, 0), prog_fd = get<u32>(a, 4), type = get<u32>(a, 8);
    vfs::File* target = proc::fd_get(proc::current(), static_cast<int>(target_fd));
    if (!target)
        return -err::BADF;
    vfs::Inode* t = target->inode;
    vfs::file_put(target);
    u32 prog_id = 0;
    if (cmd == PROG_ATTACH || prog_fd) {
        Held h;
        if (long r = hold(static_cast<int>(prog_fd), &h, Kind::Prog); r < 0)
            return r;
        prog_id = h.obj->id;
    }
    LockGuard g(g_attach_lock);
    if (cmd == PROG_ATTACH) {
        // One program per (cgroup, attach type) unless multi-attach: the newest replaces.
        for (Attachment* at = g_attachments; at; at = at->next) {
            if (at->target == t && at->attach_type == type) {
                at->prog_id = prog_id;
                return 0;
            }
        }
        auto* at = static_cast<Attachment*>(kmalloc(sizeof(Attachment)));
        if (!at)
            return -err::NOMEM;
        *at = Attachment{t, type, prog_id, g_attachments};
        g_attachments = at;
        return 0;
    }
    for (Attachment** p = &g_attachments; *p; p = &(*p)->next) {
        Attachment* at = *p;
        if (at->target == t && at->attach_type == type && (!prog_id || at->prog_id == prog_id)) {
            *p = at->next;
            kfree(at);
            return 0;
        }
    }
    return -err::NOENT;
}

long prog_query(u8* a, u64 uattr) {
    u32 target_fd = get<u32>(a, 0), type = get<u32>(a, 4);
    u64 uids = get<u64>(a, 16);
    u32 room = get<u32>(a, 24);
    vfs::File* target = proc::fd_get(proc::current(), static_cast<int>(target_fd));
    if (!target)
        return -err::BADF;
    vfs::Inode* t = target->inode;
    vfs::file_put(target);
    u32 ids[64], n = 0;
    {
        LockGuard g(g_attach_lock);
        for (Attachment* at = g_attachments; at && n < 64; at = at->next)
            if (at->target == t && at->attach_type == type)
                ids[n++] = at->prog_id;
    }
    put<u32>(a, 12, 0); // attach_flags
    put<u32>(a, 24, n); // prog_cnt
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(uattr + 12), a + 12, 4); r < 0)
        return r;
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(uattr + 24), a + 24, 4); r < 0)
        return r;
    if (uids && room) {
        if (room < n)
            return -err::NOSPC;
        if (long r = uaccess::copy_to(reinterpret_cast<void*>(uids), ids, n * 4); r < 0)
            return r;
    }
    return 0;
}

long get_next_id(Kind k, u8* a, u64 uattr) {
    u32 start = get<u32>(a, 0), best = 0;
    {
        LockGuard g(g_ids_lock);
        for (Object* o = g_objects; o; o = o->next)
            if (o->kind == k && o->id > start && (!best || o->id < best))
                best = o->id;
    }
    if (!best)
        return -err::NOENT;
    return uaccess::copy_to(reinterpret_cast<void*>(uattr + 4), &best, 4);
}

long get_fd_by_id(Kind k, const u8* a) {
    if (!proc::capable())
        return -err::PERM;
    u32 id = get<u32>(a, 0), flags = get<u32>(a, 8);
    Object* o;
    {
        LockGuard g(g_ids_lock);
        o = find_id(k, id);
        if (o)
            o->on_open(nullptr); // keep it alive across the install
    }
    if (!o)
        return -err::NOENT;
    long fd = install(o, flags);
    o->on_release(nullptr); // the new file holds its own reference
    return fd;
}

long info_by_fd(const u8* a) {
    vfs::File* f = proc::fd_get(proc::current(), static_cast<int>(get<u32>(a, 0)));
    if (!f)
        return -err::BADF;
    Object* o = object_of(f);
    u32 len = get<u32>(a, 4);
    u64 uinfo = get<u64>(a, 8);
    u8 info[96] = {};
    u32 size = 0;
    if (!o) {
        vfs::file_put(f);
        return -err::INVAL;
    }
    if (o->kind == Kind::Map) {
        auto* m = static_cast<Map*>(o);
        put<u32>(info, 0, m->type);
        put<u32>(info, 4, m->id);
        put<u32>(info, 8, m->key_size);
        put<u32>(info, 12, m->value_size);
        put<u32>(info, 16, m->max_entries);
        put<u32>(info, 20, m->flags);
        memcpy(info + 24, m->name, 16);
        size = 80; // struct bpf_map_info
    } else if (o->kind == Kind::Prog) {
        auto* p = static_cast<Prog*>(o);
        put<u32>(info, 0, p->type);
        put<u32>(info, 4, p->id);
        put<u32>(info, 20, p->insn_cnt * 8); // xlated_prog_len
        put<u32>(info, 48, p->created_by_uid);
        memcpy(info + 64, p->name, 16);
        size = 96;
    } else {
        put<u32>(info, 8 + (o->kind == Kind::Btf ? 4 : 0), o->id);
        size = 24;
    }
    vfs::file_put(f);
    u32 n = len < size ? len : size;
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(uinfo), info, n); r < 0)
        return r;
    return 0;
}

long btf_load(const u8* a) {
    if (!proc::capable())
        return -err::PERM;
    u64 ubtf = get<u64>(a, 0);
    u32 size = get<u32>(a, 16);
    if (!ubtf || size == 0 || size > (16u << 20))
        return -err::INVAL;
    u16 magic;
    if (long r = uaccess::copy_from(&magic, reinterpret_cast<const void*>(ubtf), 2); r < 0)
        return r;
    if (magic != 0xeB9F)
        return -err::INVAL;
    auto* b = new Blob(Kind::Btf);
    b->size = size;
    long fd = install(b);
    if (fd < 0)
        delete b;
    return fd;
}

long link_create(const u8* a) {
    if (!proc::capable())
        return -err::PERM;
    u32 prog_fd = get<u32>(a, 0), type = get<u32>(a, 8);
    Held h;
    if (long r = hold(static_cast<int>(prog_fd), &h, Kind::Prog); r < 0)
        return r;
    auto* l = new Blob(Kind::Link);
    l->prog_id = h.obj->id;
    l->attach_type = type;
    long fd = install(l);
    if (fd < 0)
        delete l;
    return fd;
}

long sys_bpf(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return sys_bpf_call(static_cast<int>(a0), a1, static_cast<u32>(a2));
}

// A bpffs directory: a RAM directory reporting BPF_FS_MAGIC.
class FsRoot : public ramfs::Dir {
public:
    long statfs(FsStats* out) override {
        long r = ramfs::Dir::statfs(out);
        out->type = BPF_FS_MAGIC;
        return r;
    }
};

} // namespace

long sys_bpf_call(int cmd, u64 uattr, u32 size) {
    u8 a[ATTR_SIZE] = {};
    if (size > ATTR_SIZE) {
        // Newer fields we don't know must be zero (Linux: E2BIG otherwise).
        u8 extra[64];
        for (u32 off = ATTR_SIZE; off < size; off += sizeof(extra)) {
            u32 n = size - off < sizeof(extra) ? size - off : sizeof(extra);
            if (long r = uaccess::copy_from(extra, reinterpret_cast<const void*>(uattr + off), n); r < 0)
                return r;
            for (u32 i = 0; i < n; i++)
                if (extra[i])
                    return -err::TOOBIG;
        }
        size = ATTR_SIZE;
    }
    if (long r = uaccess::copy_from(a, reinterpret_cast<const void*>(uattr), size); r < 0)
        return r;
    switch (cmd) {
    case MAP_CREATE: return map_create(a);
    case MAP_LOOKUP_ELEM: case MAP_UPDATE_ELEM: case MAP_DELETE_ELEM: case MAP_GET_NEXT_KEY:
    case MAP_LOOKUP_AND_DELETE_ELEM:
        return map_elem(cmd, a);
    case MAP_FREEZE: {
        Held h;
        if (long r = hold(static_cast<int>(get<u32>(a, 0)), &h, Kind::Map); r < 0)
            return r;
        static_cast<Map*>(h.obj)->frozen = true;
        return 0;
    }
    case PROG_LOAD: return prog_load(a);
    case OBJ_PIN: return obj_pin(a);
    case OBJ_GET: return obj_get(a);
    case PROG_ATTACH: case PROG_DETACH: return prog_attach(cmd, a);
    case PROG_QUERY: return prog_query(a, uattr);
    case PROG_TEST_RUN: return -ENOTSUPP;
    case PROG_GET_NEXT_ID: return get_next_id(Kind::Prog, a, uattr);
    case MAP_GET_NEXT_ID: return get_next_id(Kind::Map, a, uattr);
    case BTF_GET_NEXT_ID: return get_next_id(Kind::Btf, a, uattr);
    case LINK_GET_NEXT_ID: return get_next_id(Kind::Link, a, uattr);
    case PROG_GET_FD_BY_ID: return get_fd_by_id(Kind::Prog, a);
    case MAP_GET_FD_BY_ID: return get_fd_by_id(Kind::Map, a);
    case BTF_GET_FD_BY_ID: return get_fd_by_id(Kind::Btf, a);
    case LINK_GET_FD_BY_ID: return get_fd_by_id(Kind::Link, a);
    case OBJ_GET_INFO_BY_FD: return info_by_fd(a);
    case BTF_LOAD: return btf_load(a);
    case LINK_CREATE: return link_create(a);
    case LINK_DETACH: case LINK_UPDATE: return 0;
    default: return -err::INVAL;
    }
}

void init() { vfs::mkdir_p("/sys/fs/bpf", 0755); }

vfs::Inode* fs_mount() {
    auto* root = new FsRoot;
    root->mode = vfs::S_IFDIR | 01777; // Linux: sticky, world-writable
    return root;
}

void register_syscalls() { syscall::set(321, sys_bpf, "bpf"); }

} // namespace bpf
