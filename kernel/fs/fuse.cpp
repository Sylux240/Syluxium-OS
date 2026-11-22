#include <core/log.hpp>
#include <fs/fuse.hpp>
#include <fs/poll.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/vmm.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

// How it works: mount(2) binds a connection to the /dev/fuse file the server
// opened. Every file system operation becomes a request (header + arguments)
// queued on the connection; the server read()s it, does the work and write()s
// a reply with the same unique id, which wakes the waiting caller.
//
// The VFS keeps inodes for as long as the system runs, so the server never
// gets FORGET: a node id once looked up stays valid. Directory entries and
// attributes are cached for as long as the server's replies say they may be
// (squashfuse: forever), so walking paths and loading libraries from an
// AppImage costs one round trip per name, not one per access. Pages of files
// that are mapped (programs, libraries) are cached and shared between
// processes, like the disk file systems' page cache.
namespace fuse {

namespace {

// ---- Protocol (Linux include/uapi/linux/fuse.h) ----
constexpr u32 KERNEL_MAJOR = 7, KERNEL_MINOR = 31;
constexpr u64 ROOT_ID = 1;
enum : u32 {
    OP_LOOKUP = 1, OP_FORGET = 2, OP_GETATTR = 3, OP_SETATTR = 4, OP_READLINK = 5, OP_SYMLINK = 6, OP_MKNOD = 8,
    OP_MKDIR = 9, OP_UNLINK = 10, OP_RMDIR = 11, OP_RENAME = 12, OP_LINK = 13, OP_OPEN = 14, OP_READ = 15,
    OP_WRITE = 16, OP_STATFS = 17, OP_RELEASE = 18, OP_FLUSH = 25, OP_INIT = 26, OP_OPENDIR = 27,
    OP_READDIR = 28, OP_RELEASEDIR = 29, OP_CREATE = 35,
};
constexpr u32 FATTR_MODE = 1, FATTR_UID = 2, FATTR_GID = 4, FATTR_SIZE = 8, FATTR_ATIME = 16, FATTR_MTIME = 32;
constexpr u32 FOPEN_DIRECT_IO = 1;
constexpr u32 INIT_ASYNC_READ = 1, INIT_BIG_WRITES = 32;
constexpr u32 NOTIFY_INVAL_INODE = 2, NOTIFY_INVAL_ENTRY = 3;
constexpr u64 DEV_IOC_CLONE = 0x8004e500; // _IOR(229, 0, uint32_t)
constexpr u32 MIN_READ_BUFFER = 8192;
constexpr u32 IO_CHUNK = 64 * KiB; // largest read/write per request
constexpr u32 FUSE_SUPER_MAGIC = 0x65735546;
// errno values only FUSE uses here (Linux numbers)
constexpr long E_PROTO = 71, E_CONNABORTED = 103, E_NOTCONN = 107, E_CONNREFUSED = 111;

struct InHeader {
    u32 len, opcode;
    u64 unique, nodeid;
    u32 uid, gid, pid;
    u16 total_extlen, padding;
};
struct OutHeader {
    u32 len;
    i32 error;
    u64 unique;
};
struct Attr {
    u64 ino, size, blocks, atime, mtime, ctime;
    u32 atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags;
};
struct EntryOut {
    u64 nodeid, generation, entry_valid, attr_valid;
    u32 entry_valid_nsec, attr_valid_nsec;
    Attr attr;
};
struct AttrOut {
    u64 attr_valid;
    u32 attr_valid_nsec, dummy;
    Attr attr;
};
struct GetattrIn { u32 getattr_flags, dummy; u64 fh; };
struct OpenIn { u32 flags, open_flags; };
struct OpenOut { u64 fh; u32 open_flags, padding; };
struct ReadIn { u64 fh, offset; u32 size, read_flags; u64 lock_owner; u32 flags, padding; };
struct WriteIn { u64 fh, offset; u32 size, write_flags; u64 lock_owner; u32 flags, padding; };
struct WriteOut { u32 size, padding; };
struct ReleaseIn { u64 fh; u32 flags, release_flags; u64 lock_owner; };
struct FlushIn { u64 fh; u32 unused, padding; u64 lock_owner; };
struct MknodIn { u32 mode, rdev, umask, padding; };
struct MkdirIn { u32 mode, umask; };
struct CreateIn { u32 flags, mode, umask, open_flags; };
struct RenameIn { u64 newdir; };
struct LinkIn { u64 oldnodeid; };
struct SetattrIn {
    u32 valid, padding;
    u64 fh, size, lock_owner, atime, mtime, ctime;
    u32 atimensec, mtimensec, ctimensec, mode, unused4, uid, gid, unused5;
};
struct InitIn { u32 major, minor, max_readahead, flags; };
struct InitOut {
    u32 major, minor, max_readahead, flags;
    u16 max_background, congestion_threshold;
    u32 max_write, time_gran;
    u16 max_pages, map_alignment;
    u32 flags2;
    u32 unused[7];
};
struct Kstatfs {
    u64 blocks, bfree, bavail, files, ffree;
    u32 bsize, namelen, frsize, padding;
    u32 spare[6];
};
struct Dirent { u64 ino, off; u32 namelen, type; };
struct InvalInodeOut { u64 ino; i64 off, len; };
struct InvalEntryOut { u64 parent; u32 namelen, flags; };
static_assert(sizeof(InHeader) == 40 && sizeof(OutHeader) == 16 && sizeof(Attr) == 88);
static_assert(sizeof(EntryOut) == 128 && sizeof(AttrOut) == 104 && sizeof(ReadIn) == 40);
static_assert(sizeof(SetattrIn) == 88 && sizeof(InitOut) == 64 && sizeof(Kstatfs) == 80);

// Callers hand in user buffers (read/write) or kernel ones (exec, page faults).
bool is_kernel(const void* p) { return reinterpret_cast<uptr>(p) >= mm::USER_END; }
long copy_out(void* dst, const void* src, usize n) {
    if (is_kernel(dst)) {
        memcpy(dst, src, n);
        return 0;
    }
    return uaccess::copy_to(dst, src, n);
}
long copy_in(void* dst, const void* src, usize n) {
    if (is_kernel(src)) {
        memcpy(dst, src, n);
        return 0;
    }
    return uaccess::copy_from(dst, src, n);
}

// Server timeouts (seconds + nanoseconds) as an absolute time; squashfuse
// says "forever" with huge values.
u64 expiry(u64 sec, u32 nsec) {
    if (sec > 100'000'000ull)
        return ~0ull;
    return clock::now_ns() + sec * 1'000'000'000ull + nsec;
}

struct Conn;
class Node;

struct Request {
    Request* next = nullptr;
    u64 unique = 0;
    u8* in = nullptr;
    u32 in_len = 0;
    void* out = nullptr; // where the reply's payload goes (kernel memory)
    u32 out_max = 0, out_len = 0;
    i32 error = 0;
    bool done = false;
    bool background = false;                    // nobody waits: freed once answered (INIT, RELEASE)
    void (*on_reply)(Conn*, Request*) = nullptr; // background requests' handler
};

void free_request(Request* r) {
    kfree(r->in);
    if (r->background)
        kfree(r->out);
    delete r;
}

struct Conn {
    sched::WaitQueue wq; // its lock guards the queues and the flags
    Request* pending = nullptr;
    Request* pending_tail = nullptr;
    Request* processing = nullptr;
    u64 next_unique = 2;
    bool connected = true, initialized = false, mounted = false;
    i32 init_error = 0;
    u32 dev_files = 1; // /dev/fuse files attached (the server's, clones)
    u32 minor = 0, max_write = 4096, max_read = IO_CHUNK;
    u32 user_id = 0, group_id = 0;
    bool allow_other = false;
    u64 dev = 0;
    Node* root = nullptr;

    // Node table: node id -> inode. Entries are only ever added.
    static constexpr usize BUCKETS = 512;
    Node* table[BUCKETS] = {};
    Spinlock table_lock;
};

// Pieces of a request's argument block.
struct Arg {
    const void* p;
    u32 n;
};

// Queue a request; the reply wakes us.
long call(Conn* c, u32 opcode, u64 nodeid, const Arg* args, int nargs, void* out, u32 out_max, u32* out_len = nullptr) {
    if (opcode != OP_INIT)
        c->wq.wait_until([&] { return c->initialized || !c->connected; });
    u32 len = sizeof(InHeader);
    for (int i = 0; i < nargs; i++)
        len += args[i].n;
    auto* r = new Request;
    r->in = static_cast<u8*>(kmalloc(len));
    if (!r->in) {
        delete r;
        return -err::NOMEM;
    }
    r->in_len = len;
    r->out = out;
    r->out_max = out_max;
    const proc::Cred& cred = proc::cred();
    proc::Process* p = proc::current();
    auto* h = reinterpret_cast<InHeader*>(r->in);
    *h = InHeader{len, opcode, 0, nodeid, cred.fsuid, cred.fsgid, static_cast<u32>(p ? p->pid : 0), 0, 0};
    u32 at = sizeof(InHeader);
    for (int i = 0; i < nargs; i++) {
        if (args[i].n && copy_in(r->in + at, args[i].p, args[i].n) < 0) {
            free_request(r);
            return -err::FAULT;
        }
        at += args[i].n;
    }
    {
        LockGuard guard(c->wq.lock());
        if (!c->connected || (c->init_error && opcode != OP_INIT)) {
            r->error = c->connected ? -E_CONNREFUSED : -E_NOTCONN;
            r->done = true;
        } else {
            r->unique = h->unique = c->next_unique++;
            if (c->pending_tail)
                c->pending_tail->next = r;
            else
                c->pending = r;
            c->pending_tail = r;
        }
    }
    if (!r->done) {
        c->wq.wake_all();
        poll::notify();
        c->wq.wait_until([&] { return r->done; });
    }
    long result = r->error;
    if (out_len)
        *out_len = r->out_len;
    free_request(r);
    return result;
}

// A request whose reply nobody waits for.
void call_background(Conn* c, u32 opcode, u64 nodeid, const void* arg, u32 arg_len, u32 out_max,
                     void (*on_reply)(Conn*, Request*)) {
    u32 len = sizeof(InHeader) + arg_len;
    auto* r = new Request;
    r->background = true;
    r->on_reply = on_reply;
    r->in = static_cast<u8*>(kmalloc(len));
    r->out = out_max ? kzalloc(out_max) : nullptr;
    r->out_max = out_max;
    if (!r->in || (out_max && !r->out)) {
        free_request(r);
        return;
    }
    r->in_len = len;
    const proc::Cred& cred = proc::cred();
    proc::Process* p = proc::current();
    auto* h = reinterpret_cast<InHeader*>(r->in);
    *h = InHeader{len, opcode, 0, nodeid, cred.fsuid, cred.fsgid, static_cast<u32>(p ? p->pid : 0), 0, 0};
    memcpy(r->in + sizeof(InHeader), arg, arg_len);
    {
        LockGuard guard(c->wq.lock());
        if (!c->connected) {
            r->done = true;
        } else {
            r->unique = h->unique = c->next_unique++;
            if (c->pending_tail)
                c->pending_tail->next = r;
            else
                c->pending = r;
            c->pending_tail = r;
        }
    }
    if (r->done) {
        free_request(r);
        return;
    }
    c->wq.wake_all();
    poll::notify();
}

// A request is answered (or the connection ended): wake its caller, or run
// a background request's handler. Called without the lock.
void complete(Conn* c, Request* r) {
    if (r->background) {
        if (r->on_reply)
            r->on_reply(c, r);
        free_request(r);
        return;
    }
    {
        LockGuard guard(c->wq.lock());
        r->done = true;
    }
    c->wq.wake_all();
}

// The server is gone or the file system was unmounted: every request fails.
void abort(Conn* c) {
    Request* list = nullptr;
    {
        LockGuard guard(c->wq.lock());
        if (!c->connected)
            return;
        c->connected = false;
        for (Request* r = c->pending; r;) {
            Request* next = r->next;
            r->error = -E_NOTCONN;
            r->next = list;
            list = r;
            r = next;
        }
        for (Request* r = c->processing; r;) {
            Request* next = r->next;
            r->error = -E_CONNABORTED;
            r->next = list;
            list = r;
            r = next;
        }
        c->pending = c->pending_tail = c->processing = nullptr;
    }
    while (list) {
        Request* next = list->next;
        complete(c, list);
        list = next;
    }
    c->wq.wake_all();
    poll::notify();
}

void root_attr_reply(Conn* c, Request* r);

void init_reply(Conn* c, Request* r) {
    auto* o = static_cast<InitOut*>(r->out);
    i32 e = r->error;
    if (!e && (r->out_len < 8 || o->major != KERNEL_MAJOR || o->minor < 12))
        e = -E_PROTO;
    {
        LockGuard guard(c->wq.lock());
        if (!e) {
            c->minor = min(o->minor, KERNEL_MINOR);
            if (r->out_len >= 24 && o->max_write >= 4096)
                c->max_write = o->max_write;
        } else {
            c->init_error = e;
        }
        c->initialized = true;
    }
    if (e)
        klog::warn("fuse: the server rejected INIT (%d)", e);
    c->wq.wake_all();
    // The root's real attributes (mount(2) only gave its type).
    if (!e) {
        GetattrIn in{};
        call_background(c, OP_GETATTR, ROOT_ID, &in, sizeof(in), sizeof(AttrOut), root_attr_reply);
    }
}

// ---- Inodes ----

struct OpenState {
    u64 fh = 0;
    i32 error = 0;
    u32 open_flags = 0;
};

struct Dentry {
    Dentry* next;
    Node* node; // nullptr: known not to exist
    u64 expires;
    u16 len;
    char name[1];
};

struct ListEntry {
    u64 ino;
    u8 type;
    char* name;
};

Node* get_node(Conn* c, u64 nodeid, const Attr& a, u64 attr_expires);

class Node : public vfs::Inode {
public:
    Conn* conn;
    u64 nodeid;
    Node* hash_next = nullptr;

    Node(Conn* c, u64 id, u32 type_mode) : conn(c), nodeid(id) {
        mode = type_mode;
        ino = id;
        dev = c->dev;
        atime = mtime = ctime = vfs::now();
    }

    // Called with m_lock held or before the node is visible.
    void apply_attr(const Attr& a, u64 expires) {
        u32 type = a.mode & vfs::S_IFMT;
        bool changed = vfs::is_reg(mode) && (a.size != size || static_cast<i64>(a.mtime) != mtime.sec ||
                                             static_cast<i64>(a.mtimensec) != mtime.nsec);
        mode = type ? a.mode : ((mode & vfs::S_IFMT) | (a.mode & 07777));
        mode &= ~(vfs::S_ISUID | vfs::S_ISGID); // mounted nosuid: a user's file system makes nobody root
        ino = a.ino ? a.ino : nodeid;
        size = a.size;
        nlink = a.nlink ? a.nlink : 1;
        uid = a.uid;
        gid = a.gid;
        rdev = ((a.rdev >> 8) & 0xfff) << 8 | (a.rdev & 0xff);
        atime = {static_cast<i64>(a.atime), a.atimensec};
        mtime = {static_cast<i64>(a.mtime), a.mtimensec};
        ctime = {static_cast<i64>(a.ctime), a.ctimensec};
        m_attr_expires = expires;
        if (changed)
            m_cache_stale = true;
    }

    // ---- attributes ----
    bool access_ok() const {
        if (conn->allow_other)
            return true;
        const proc::Cred& cr = proc::cred();
        return cr.fsuid == 0 || cr.fsuid == conn->user_id;
    }

    long refresh_attr(bool force) {
        if (!force && clock::now_ns() < __atomic_load_n(&m_attr_expires, __ATOMIC_RELAXED))
            return 0;
        GetattrIn in{};
        AttrOut out{};
        Arg args[] = {{&in, sizeof(in)}};
        u32 got = 0;
        long r = call(conn, OP_GETATTR, nodeid, args, 1, &out, sizeof(out), &got);
        if (r < 0)
            return r;
        if (got < sizeof(AttrOut))
            return -err::IO;
        LockGuard guard(m_lock);
        apply_attr(out.attr, expiry(out.attr_valid, out.attr_valid_nsec));
        drop_cache_if_stale();
        return 0;
    }

    long revalidate() override {
        if (!conn->connected)
            return -E_NOTCONN;
        if (!access_ok())
            return -err::ACCES;
        return refresh_attr(false);
    }

    long statfs(FsStats* out) override {
        Kstatfs st{};
        u32 got = 0;
        long r = call(conn, OP_STATFS, nodeid, nullptr, 0, &st, sizeof(st), &got);
        if (r == -err::NOSYS || (r == 0 && got < 48))
            st = Kstatfs{0, 0, 0, 0, 0, 512, 255, 512, 0, {}};
        else if (r < 0)
            return r;
        *out = FsStats{FUSE_SUPER_MAGIC, st.bsize ? st.bsize : 512, st.blocks, st.bavail, st.files, st.ffree};
        return 0;
    }

    // ---- directories ----
    vfs::Inode* lookup(const char* name, usize len) override {
        if (!vfs::is_dir(mode) || !access_ok() || len > 255)
            return nullptr;
        u64 now = clock::now_ns();
        {
            LockGuard guard(m_lock);
            for (Dentry* d = m_dentries; d; d = d->next)
                if (d->len == len && memcmp(d->name, name, len) == 0 && d->expires > now)
                    return d->node;
        }
        EntryOut e{};
        Arg args[] = {{name, static_cast<u32>(len)}, {"", 1}};
        u32 got = 0;
        long r = call(conn, OP_LOOKUP, nodeid, args, 2, &e, sizeof(e), &got);
        if (r == -err::NOENT || (r == 0 && got >= 8 && e.nodeid == 0)) {
            if (r == 0 && (e.entry_valid || e.entry_valid_nsec))
                remember(name, len, nullptr, expiry(e.entry_valid, e.entry_valid_nsec));
            return nullptr;
        }
        if (r < 0 || got < sizeof(EntryOut))
            return nullptr;
        Node* n = get_node(conn, e.nodeid, e.attr, expiry(e.attr_valid, e.attr_valid_nsec));
        if (!n)
            return nullptr;
        if (n != this)
            n->parent = this;
        remember(name, len, n, expiry(e.entry_valid, e.entry_valid_nsec));
        return n;
    }

    long readdir(u64 index, vfs::DirEntry* out) override {
        if (!vfs::is_dir(mode))
            return -err::NOTDIR;
        if (!conn->connected)
            return -E_NOTCONN;
        if (index == 0) {
            out->ino = ino;
            out->type = vfs::DT_DIR;
            snprintf(out->name, sizeof(out->name), ".");
            if (long r = fetch_listing(); r < 0) // a fresh listing for every pass over the directory
                return r;
            return 1;
        }
        if (index == 1) {
            out->ino = parent ? parent->ino : ino;
            out->type = vfs::DT_DIR;
            snprintf(out->name, sizeof(out->name), "..");
            return 1;
        }
        LockGuard guard(m_lock);
        if (!m_list || index - 2 >= m_list_count)
            return 0;
        const ListEntry& e = m_list[index - 2];
        out->ino = e.ino;
        out->type = e.type;
        snprintf(out->name, sizeof(out->name), "%s", e.name);
        return 1;
    }

    long create(const char* name, usize len, u32 new_mode, const char* target, vfs::Inode** out) override {
        if (!vfs::is_dir(mode))
            return -err::NOTDIR;
        if (len > 255)
            return -err::NAMETOOLONG;
        EntryOut e{};
        u32 got = 0;
        long r;
        u32 type = new_mode & vfs::S_IFMT;
        if (type == vfs::S_IFDIR) {
            MkdirIn in{new_mode & 07777, 0};
            Arg args[] = {{&in, sizeof(in)}, {name, static_cast<u32>(len)}, {"", 1}};
            r = call(conn, OP_MKDIR, nodeid, args, 3, &e, sizeof(e), &got);
        } else if (type == vfs::S_IFLNK) {
            Arg args[] = {{name, static_cast<u32>(len)}, {"", 1}, {target, static_cast<u32>(strlen(target) + 1)}};
            r = call(conn, OP_SYMLINK, nodeid, args, 3, &e, sizeof(e), &got);
        } else {
            MknodIn in{new_mode, 0, 0, 0};
            Arg args[] = {{&in, sizeof(in)}, {name, static_cast<u32>(len)}, {"", 1}};
            r = call(conn, OP_MKNOD, nodeid, args, 3, &e, sizeof(e), &got);
            if (r == -err::NOSYS && type == vfs::S_IFREG) {
                // Servers that only implement create(): create + release.
                struct {
                    EntryOut entry;
                    OpenOut open;
                } reply{};
                CreateIn cin{vfs::O_WRONLY | vfs::O_CREAT | vfs::O_EXCL, new_mode, 0, 0};
                Arg cargs[] = {{&cin, sizeof(cin)}, {name, static_cast<u32>(len)}, {"", 1}};
                r = call(conn, OP_CREATE, nodeid, cargs, 3, &reply, sizeof(reply), &got);
                if (r == 0 && got >= sizeof(reply)) {
                    e = reply.entry;
                    ReleaseIn rin{reply.open.fh, vfs::O_WRONLY, 0, 0};
                    call_background(conn, OP_RELEASE, e.nodeid, &rin, sizeof(rin), 0, nullptr);
                    got = sizeof(EntryOut);
                }
            }
        }
        if (r < 0)
            return r;
        if (got < sizeof(EntryOut) || !e.nodeid)
            return -err::IO;
        Node* n = get_node(conn, e.nodeid, e.attr, expiry(e.attr_valid, e.attr_valid_nsec));
        if (!n)
            return -err::NOMEM;
        n->parent = this;
        remember(name, len, n, expiry(e.entry_valid, e.entry_valid_nsec));
        changed();
        if (out)
            *out = n;
        return 0;
    }

    long unlink(const char* name, usize len, bool remove_dir) override {
        if (!vfs::is_dir(mode))
            return -err::NOTDIR;
        Node* victim = static_cast<Node*>(lookup(name, len));
        Arg args[] = {{name, static_cast<u32>(len)}, {"", 1}};
        long r = call(conn, remove_dir ? OP_RMDIR : OP_UNLINK, nodeid, args, 2, nullptr, 0);
        forget_name(name, len);
        changed();
        if (r == 0 && victim)
            victim->m_attr_expires = 0; // its link count changed
        return r;
    }

    long rename(const char* old_name, usize old_len, vfs::Inode* new_dir, const char* new_name,
                usize new_len) override {
        auto* nd = static_cast<Node*>(new_dir);
        if (!is_ours(new_dir))
            return -err::XDEV;
        RenameIn in{nd->nodeid};
        Arg args[] = {{&in, sizeof(in)}, {old_name, static_cast<u32>(old_len)}, {"", 1},
                      {new_name, static_cast<u32>(new_len)}, {"", 1}};
        long r = call(conn, OP_RENAME, nodeid, args, 5, nullptr, 0);
        forget_name(old_name, old_len);
        nd->forget_name(new_name, new_len);
        changed();
        nd->changed();
        return r;
    }

    long link(const char* name, usize len, vfs::Inode* target) override {
        if (!is_ours(target))
            return -err::XDEV;
        LinkIn in{static_cast<Node*>(target)->nodeid};
        EntryOut e{};
        u32 got = 0;
        Arg args[] = {{&in, sizeof(in)}, {name, static_cast<u32>(len)}, {"", 1}};
        long r = call(conn, OP_LINK, nodeid, args, 3, &e, sizeof(e), &got);
        if (r < 0)
            return r;
        if (got >= sizeof(EntryOut) && e.nodeid) {
            Node* n = get_node(conn, e.nodeid, e.attr, expiry(e.attr_valid, e.attr_valid_nsec));
            if (n)
                remember(name, len, n, expiry(e.entry_valid, e.entry_valid_nsec));
        }
        changed();
        return 0;
    }

    // ---- symlinks ----
    isize readlink(char* buf, usize bufsize) override {
        if (!vfs::is_lnk(mode))
            return -err::INVAL;
        {
            LockGuard guard(m_lock);
            if (m_link) {
                usize n = min(strlen(m_link), bufsize);
                memcpy(buf, m_link, n);
                return static_cast<isize>(n);
            }
        }
        auto* target = static_cast<char*>(kmalloc(4097));
        if (!target)
            return -err::NOMEM;
        u32 got = 0;
        long r = call(conn, OP_READLINK, nodeid, nullptr, 0, target, 4096, &got);
        if (r < 0) {
            kfree(target);
            return r;
        }
        target[got] = 0;
        usize n = min<usize>(got, bufsize);
        memcpy(buf, target, n);
        LockGuard guard(m_lock);
        if (!m_link)
            m_link = target; // a symlink's target never changes
        else
            kfree(target);
        return static_cast<isize>(n);
    }

    // ---- regular files ----
    void on_open(vfs::File* f) override {
        if (!vfs::is_reg(mode) || (f->flags & vfs::O_PATH))
            return;
        auto* s = new OpenState;
        f->private_data = s;
        refresh_attr(false);
        OpenIn in{f->flags & ~(vfs::O_CREAT | vfs::O_EXCL | vfs::O_NOCTTY | vfs::O_TRUNC), 0};
        OpenOut out{};
        Arg args[] = {{&in, sizeof(in)}};
        long r = call(conn, OP_OPEN, nodeid, args, 1, &out, sizeof(out));
        if (r < 0)
            s->error = static_cast<i32>(r);
        s->fh = out.fh;
        s->open_flags = out.open_flags;
    }

    void on_release(vfs::File* f) override {
        auto* s = static_cast<OpenState*>(f->private_data);
        if (!s)
            return;
        f->private_data = nullptr;
        if (!s->error && conn->connected) {
            if ((f->flags & vfs::O_ACCMODE) != vfs::O_RDONLY) {
                FlushIn fl{s->fh, 0, 0, 0};
                Arg args[] = {{&fl, sizeof(fl)}};
                call(conn, OP_FLUSH, nodeid, args, 1, nullptr, 0);
            }
            ReleaseIn in{s->fh, f->flags, 0, 0};
            call_background(conn, OP_RELEASE, nodeid, &in, sizeof(in), 0, nullptr);
        }
        delete s;
    }

    isize read(u64 offset, void* buf, usize count) override {
        if (vfs::is_dir(mode))
            return -err::ISDIR;
        u64 fh;
        if (long r = shared_fh(false, &fh); r < 0)
            return r;
        return do_read(fh, offset, buf, count, true);
    }

    isize read_file(vfs::File* f, u64 offset, void* buf, usize count) override {
        auto* s = static_cast<OpenState*>(f->private_data);
        if (!s)
            return read(offset, buf, count);
        if (s->error)
            return s->error;
        return do_read(s->fh, offset, buf, count, !(s->open_flags & FOPEN_DIRECT_IO));
    }

    isize write(u64 offset, const void* buf, usize count) override {
        if (vfs::is_dir(mode))
            return -err::ISDIR;
        u64 fh;
        if (long r = shared_fh(true, &fh); r < 0)
            return r;
        return do_write(fh, offset, buf, count, 0);
    }

    isize write_file(vfs::File* f, u64 offset, const void* buf, usize count) override {
        auto* s = static_cast<OpenState*>(f->private_data);
        if (!s)
            return write(offset, buf, count);
        if (s->error)
            return s->error;
        return do_write(s->fh, offset, buf, count, f->flags);
    }

    long truncate(u64 new_size) override {
        SetattrIn in{};
        in.valid = FATTR_SIZE;
        in.size = new_size;
        return setattr(in);
    }

    long set_owner(u32 new_mode, u32 new_uid, u32 new_gid) override {
        SetattrIn in{};
        if ((new_mode & 07777) != (mode & 07777)) {
            in.valid |= FATTR_MODE;
            in.mode = (mode & vfs::S_IFMT) | (new_mode & 07777);
        }
        if (new_uid != uid) {
            in.valid |= FATTR_UID;
            in.uid = new_uid;
        }
        if (new_gid != gid) {
            in.valid |= FATTR_GID;
            in.gid = new_gid;
        }
        return in.valid ? setattr(in) : 0;
    }

    long set_times(vfs::Timespec access, vfs::Timespec modify) override {
        SetattrIn in{};
        in.valid = FATTR_ATIME | FATTR_MTIME;
        in.atime = static_cast<u64>(access.sec);
        in.atimensec = static_cast<u32>(access.nsec);
        in.mtime = static_cast<u64>(modify.sec);
        in.mtimensec = static_cast<u32>(modify.nsec);
        return setattr(in);
    }

    // Pages of mapped files (programs, libraries), shared by every process
    // that maps them.
    long cache_page(u64 offset, u64* phys) override {
        if (!vfs::is_reg(mode) || (offset & 4095))
            return -err::INVAL;
        u64 fh;
        if (long r = shared_fh(false, &fh); r < 0)
            return r;
        LockGuard guard(m_lock);
        drop_cache_if_stale();
        if (offset >= size)
            return -err::FAULT;
        u32 index = static_cast<u32>(offset / 4096);
        if (index >= m_cache_pages) {
            u32 want = static_cast<u32>((size + 4095) / 4096);
            if (want <= index)
                return -err::FAULT;
            auto* grown = static_cast<u64*>(kzalloc(want * sizeof(u64)));
            if (!grown)
                return -err::NOMEM;
            if (m_cache)
                memcpy(grown, m_cache, m_cache_pages * sizeof(u64));
            kfree(m_cache);
            m_cache = grown;
            m_cache_pages = want;
        }
        if (!m_cache[index]) {
            u64 p = pmm::alloc_page(true);
            if (!p)
                return -err::NOMEM;
            usize n = min<u64>(4096, size - offset);
            isize got = do_read(fh, offset, mm::phys_to_virt<u8>(p), n, false);
            if (got < 0) {
                pmm::free_page(p);
                return -err::IO;
            }
            m_cache[index] = p; // the cache's own reference
        }
        *phys = m_cache[index];
        return 0;
    }

    void drop_cache() {
        if (m_cache)
            for (u32 i = 0; i < m_cache_pages; i++)
                if (m_cache[i])
                    vmm::release_user_page(m_cache[i]); // mappings keep their own references
        kfree(m_cache);
        m_cache = nullptr;
        m_cache_pages = 0;
        m_cache_stale = false;
    }

    // Everything this node keeps that the server can't refresh any more.
    void teardown() {
        LockGuard guard(m_lock);
        drop_cache();
        free_listing();
        while (m_dentries) {
            Dentry* d = m_dentries;
            m_dentries = d->next;
            kfree(d);
        }
    }

    void invalidate_if_needed(const Attr& a, u64 attr_expires);

    void invalidate_data() {
        LockGuard guard(m_lock);
        m_attr_expires = 0;
        drop_cache();
    }

    void forget_name(const char* name, usize len) {
        LockGuard guard(m_lock);
        for (Dentry** pp = &m_dentries; *pp; pp = &(*pp)->next) {
            if ((*pp)->len == len && memcmp((*pp)->name, name, len) == 0) {
                Dentry* d = *pp;
                *pp = d->next;
                kfree(d);
                return;
            }
        }
    }

private:
    sched::Mutex m_lock; // the caches below (held across server calls only for page fills)
    u64 m_attr_expires = 0;
    Dentry* m_dentries = nullptr;
    u32 m_negatives = 0;
    ListEntry* m_list = nullptr;
    u32 m_list_count = 0;
    char* m_link = nullptr;
    u64* m_cache = nullptr;
    u32 m_cache_pages = 0;
    bool m_cache_stale = false;
    bool m_has_fh[2] = {};
    u64 m_fh[2] = {};

    bool is_ours(vfs::Inode* other) const;

    void drop_cache_if_stale() {
        if (m_cache_stale)
            drop_cache();
    }

    void changed() {
        LockGuard guard(m_lock);
        m_attr_expires = 0;
    }

    void remember(const char* name, usize len, Node* node, u64 expires) {
        if (expires <= clock::now_ns())
            return;
        LockGuard guard(m_lock);
        for (Dentry* d = m_dentries; d; d = d->next) {
            if (d->len == len && memcmp(d->name, name, len) == 0) {
                if (!d->node && node)
                    m_negatives--;
                else if (d->node && !node)
                    m_negatives++;
                d->node = node;
                d->expires = expires;
                return;
            }
        }
        if (!node && m_negatives >= 256)
            return; // programs probing many missing names (the dynamic loader) don't fill memory
        auto* d = static_cast<Dentry*>(kmalloc(sizeof(Dentry) + len));
        if (!d)
            return;
        memcpy(d->name, name, len);
        d->name[len] = 0;
        d->len = static_cast<u16>(len);
        d->node = node;
        d->expires = expires;
        d->next = m_dentries;
        m_dentries = d;
        if (!node)
            m_negatives++;
    }

    void free_listing() {
        for (u32 i = 0; i < m_list_count; i++)
            kfree(m_list[i].name);
        kfree(m_list);
        m_list = nullptr;
        m_list_count = 0;
    }

    long fetch_listing() {
        OpenIn oin{vfs::O_RDONLY | vfs::O_DIRECTORY, 0};
        OpenOut oout{};
        Arg oargs[] = {{&oin, sizeof(oin)}};
        if (long r = call(conn, OP_OPENDIR, nodeid, oargs, 1, &oout, sizeof(oout)); r < 0)
            return r;
        constexpr u32 BUF = 16 * KiB;
        auto* buf = static_cast<u8*>(kmalloc(BUF));
        ListEntry* list = nullptr;
        u32 count = 0, cap = 0;
        long result = buf ? 0 : -err::NOMEM;
        u64 offset = 0;
        while (result == 0) {
            ReadIn in{oout.fh, offset, BUF, 0, 0, vfs::O_RDONLY, 0};
            Arg args[] = {{&in, sizeof(in)}};
            u32 got = 0;
            if (long r = call(conn, OP_READDIR, nodeid, args, 1, buf, BUF, &got); r < 0) {
                result = r;
                break;
            }
            if (got == 0)
                break;
            u32 at = 0;
            bool any = false;
            while (at + sizeof(Dirent) <= got) {
                auto* d = reinterpret_cast<Dirent*>(buf + at);
                u32 rec = (static_cast<u32>(sizeof(Dirent)) + d->namelen + 7) & ~7u;
                if (d->namelen == 0 || d->namelen > 255 || at + sizeof(Dirent) + d->namelen > got)
                    break;
                any = true;
                offset = d->off;
                const char* name = reinterpret_cast<const char*>(d + 1);
                bool dots = (d->namelen == 1 && name[0] == '.') || (d->namelen == 2 && name[0] == '.' && name[1] == '.');
                if (!dots) {
                    if (count == cap) {
                        u32 ncap = cap ? cap * 2 : 32;
                        auto* grown = static_cast<ListEntry*>(kmalloc(ncap * sizeof(ListEntry)));
                        if (!grown) {
                            result = -err::NOMEM;
                            break;
                        }
                        if (list)
                            memcpy(grown, list, count * sizeof(ListEntry));
                        kfree(list);
                        list = grown;
                        cap = ncap;
                    }
                    char* copy = static_cast<char*>(kmalloc(d->namelen + 1));
                    if (!copy) {
                        result = -err::NOMEM;
                        break;
                    }
                    memcpy(copy, name, d->namelen);
                    copy[d->namelen] = 0;
                    list[count++] = ListEntry{d->ino, static_cast<u8>(d->type), copy};
                }
                at += rec;
            }
            if (!any)
                break;
        }
        kfree(buf);
        ReleaseIn rin{oout.fh, vfs::O_RDONLY | vfs::O_DIRECTORY, 0, 0};
        call_background(conn, OP_RELEASEDIR, nodeid, &rin, sizeof(rin), 0, nullptr);
        LockGuard guard(m_lock);
        free_listing();
        m_list = list;
        m_list_count = count;
        return result;
    }

    // The file handle used where there's no open file (exec, page faults,
    // kernel callers): opened once per node and direction.
    long shared_fh(bool writing, u64* fh) {
        int i = writing ? 1 : 0;
        {
            LockGuard guard(m_lock);
            if (m_has_fh[i]) {
                *fh = m_fh[i];
                return 0;
            }
        }
        OpenIn in{writing ? vfs::O_RDWR : vfs::O_RDONLY, 0};
        OpenOut out{};
        Arg args[] = {{&in, sizeof(in)}};
        if (long r = call(conn, OP_OPEN, nodeid, args, 1, &out, sizeof(out)); r < 0)
            return r;
        LockGuard guard(m_lock);
        if (m_has_fh[i]) { // opened twice at once: keep the first
            ReleaseIn rin{out.fh, in.flags, 0, 0};
            call_background(conn, OP_RELEASE, nodeid, &rin, sizeof(rin), 0, nullptr);
        } else {
            m_has_fh[i] = true;
            m_fh[i] = out.fh;
        }
        *fh = m_fh[i];
        return 0;
    }

    isize do_read(u64 fh, u64 offset, void* buf, usize count, bool clip) {
        if (clip && vfs::is_reg(mode)) { // reads stop at the size the server reported
            if (offset >= size)
                return 0;
            count = min<u64>(count, size - offset);
        }
        if (count == 0)
            return 0;
        u32 chunk_max = min(conn->max_read, IO_CHUNK);
        bool direct = is_kernel(buf);
        u8* bounce = direct ? nullptr : static_cast<u8*>(kmalloc(min<usize>(count, chunk_max)));
        if (!direct && !bounce)
            return -err::NOMEM;
        usize done = 0;
        long error = 0;
        while (done < count) {
            u32 chunk = static_cast<u32>(min<usize>(count - done, chunk_max));
            ReadIn in{fh, offset + done, chunk, 0, 0, vfs::O_RDONLY, 0};
            Arg args[] = {{&in, sizeof(in)}};
            u32 got = 0;
            u8* dst = direct ? static_cast<u8*>(buf) + done : bounce;
            if (long r = call(conn, OP_READ, nodeid, args, 1, dst, chunk, &got); r < 0) {
                error = r;
                break;
            }
            if (!direct && got && copy_out(static_cast<u8*>(buf) + done, bounce, got) < 0) {
                error = -err::FAULT;
                break;
            }
            done += got;
            if (got < chunk)
                break; // end of file
        }
        kfree(bounce);
        if (done == 0 && error)
            return error;
        return static_cast<isize>(done);
    }

    isize do_write(u64 fh, u64 offset, const void* buf, usize count, u32 flags) {
        u32 chunk_max = min(conn->max_write, IO_CHUNK);
        usize done = 0;
        long error = 0;
        while (done < count) {
            u32 chunk = static_cast<u32>(min<usize>(count - done, chunk_max));
            WriteIn in{fh, offset + done, chunk, 0, 0, flags, 0};
            WriteOut out{};
            Arg args[] = {{&in, sizeof(in)}, {static_cast<const u8*>(buf) + done, chunk}};
            if (long r = call(conn, OP_WRITE, nodeid, args, 2, &out, sizeof(out)); r < 0) {
                error = r;
                break;
            }
            done += min(out.size, chunk);
            if (out.size < chunk)
                break;
        }
        if (done) {
            LockGuard guard(m_lock);
            if (offset + done > size)
                size = offset + done;
            mtime = ctime = vfs::now();
            m_attr_expires = 0;
            drop_cache();
        }
        if (done == 0 && error)
            return error;
        return static_cast<isize>(done);
    }

    long setattr(SetattrIn& in) {
        AttrOut out{};
        u32 got = 0;
        Arg args[] = {{&in, sizeof(in)}};
        long r = call(conn, OP_SETATTR, nodeid, args, 1, &out, sizeof(out), &got);
        if (r < 0)
            return r;
        LockGuard guard(m_lock);
        if (got >= sizeof(AttrOut))
            apply_attr(out.attr, expiry(out.attr_valid, out.attr_valid_nsec));
        else
            m_attr_expires = 0;
        if (in.valid & FATTR_SIZE)
            drop_cache();
        else
            drop_cache_if_stale();
        return 0;
    }
};

// ---- Node table ----

Node* find_node(Conn* c, u64 nodeid) {
    LockGuard guard(c->table_lock);
    for (Node* n = c->table[nodeid % Conn::BUCKETS]; n; n = n->hash_next)
        if (n->nodeid == nodeid)
            return n;
    return nullptr;
}

Node* get_node(Conn* c, u64 nodeid, const Attr& a, u64 attr_expires) {
    Node* n = find_node(c, nodeid);
    if (n) {
        n->invalidate_if_needed(a, attr_expires);
        return n;
    }
    auto* fresh = new Node(c, nodeid, a.mode & vfs::S_IFMT);
    fresh->apply_attr(a, attr_expires);
    LockGuard guard(c->table_lock);
    for (Node* o = c->table[nodeid % Conn::BUCKETS]; o; o = o->hash_next) {
        if (o->nodeid == nodeid) { // made by someone else meanwhile
            delete fresh;
            return o;
        }
    }
    fresh->hash_next = c->table[nodeid % Conn::BUCKETS];
    __atomic_store_n(&c->table[nodeid % Conn::BUCKETS], fresh, __ATOMIC_RELEASE);
    return fresh;
}

bool Node::is_ours(vfs::Inode* other) const {
    // Nodes of this connection are the only inodes with its st_dev.
    return other && other->dev == conn->dev;
}

void for_each_node(Conn* c, void (*fn)(Node*)) {
    for (usize b = 0; b < Conn::BUCKETS; b++)
        for (Node* n = __atomic_load_n(&c->table[b], __ATOMIC_ACQUIRE); n; n = n->hash_next)
            fn(n);
}

// ---- /dev/fuse ----

class Device : public vfs::Inode {
public:
    Device() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0666;
        rdev = (10u << 8) | 229;
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }
    bool whole_writes() const override { return true; } // one reply per writev

    void on_release(vfs::File* f) override {
        auto* c = static_cast<Conn*>(f->private_data);
        if (!c)
            return;
        bool last;
        {
            LockGuard guard(c->wq.lock());
            last = --c->dev_files == 0;
        }
        if (last)
            abort(c); // the server is gone
    }

    u16 poll(vfs::File* f) override {
        auto* c = static_cast<Conn*>(f->private_data);
        if (!c)
            return poll::POLLOUT;
        LockGuard guard(c->wq.lock());
        if (!c->connected)
            return poll::POLLERR;
        return poll::POLLOUT | poll::POLLWRNORM | (c->pending ? poll::POLLIN | poll::POLLRDNORM : 0);
    }

    long ioctl_file(vfs::File* f, u64 request, u64 arg) override {
        if (request != DEV_IOC_CLONE)
            return -err::NOTTY;
        u32 old_fd;
        if (uaccess::get(&old_fd, reinterpret_cast<const u32*>(arg)) < 0)
            return -err::FAULT;
        vfs::File* old = proc::fd_get(proc::current(), static_cast<int>(old_fd));
        if (!old)
            return -err::BADF;
        auto* c = old->inode == this ? static_cast<Conn*>(old->private_data) : nullptr;
        vfs::file_put(old);
        if (!c || f->private_data)
            return -err::INVAL;
        LockGuard guard(c->wq.lock());
        c->dev_files++;
        f->private_data = c;
        return 0;
    }

    // The server takes the next request.
    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        auto* c = static_cast<Conn*>(f->private_data);
        if (!c)
            return -err::PERM; // not mounted yet
        if (count < MIN_READ_BUFFER)
            return -err::INVAL;
        for (;;) {
            Request* r = nullptr;
            {
                LockGuard guard(c->wq.lock());
                if (!c->connected)
                    return -err::NODEV;
                if ((r = c->pending)) {
                    c->pending = r->next;
                    if (!c->pending)
                        c->pending_tail = nullptr;
                    r->next = nullptr;
                    if (r->in_len <= count) {
                        r->next = c->processing;
                        c->processing = r;
                    }
                }
            }
            if (r) {
                if (r->in_len > count) {
                    r->error = -err::IO;
                    complete(c, r);
                    continue;
                }
                u32 n = r->in_len;
                if (copy_out(buf, r->in, n) < 0) {
                    if (unlink_processing(c, r->unique)) {
                        r->error = -err::IO;
                        complete(c, r);
                    }
                    return -err::FAULT;
                }
                return static_cast<isize>(n);
            }
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            c->wq.wait_until([&] { return c->pending || !c->connected || signal::interrupted(); });
        }
    }

    // A reply (or a notification, unique 0) from the server.
    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        auto* c = static_cast<Conn*>(f->private_data);
        if (!c)
            return -err::PERM;
        if (count < sizeof(OutHeader))
            return -err::INVAL;
        OutHeader h;
        if (copy_in(&h, buf, sizeof(h)) < 0)
            return -err::FAULT;
        if (h.len != count)
            return -err::INVAL;
        const u8* payload = static_cast<const u8*>(buf) + sizeof(OutHeader);
        usize plen = count - sizeof(OutHeader);
        if (h.unique == 0)
            return notify(c, h.error, payload, plen) < 0 ? -err::INVAL : static_cast<isize>(count);
        if (h.error > 0 || h.error < -4095)
            return -err::INVAL;
        Request* r = unlink_processing(c, h.unique);
        if (!r)
            return -err::NOENT; // answered already, or interrupted
        long e = 0;
        u32 n = 0;
        if (h.error == 0 && plen) {
            n = static_cast<u32>(min<usize>(plen, r->out_max));
            if (n && r->out)
                e = copy_in(r->out, payload, n);
        }
        r->error = e < 0 ? -err::IO : h.error;
        r->out_len = h.error ? 0 : n;
        complete(c, r);
        return e < 0 ? -err::FAULT : static_cast<isize>(count);
    }

private:
    static Request* unlink_processing(Conn* c, u64 unique) {
        LockGuard guard(c->wq.lock());
        for (Request** pp = &c->processing; *pp; pp = &(*pp)->next) {
            if ((*pp)->unique == unique) {
                Request* r = *pp;
                *pp = r->next;
                r->next = nullptr;
                return r;
            }
        }
        return nullptr;
    }

    // Cache invalidations the server asks for; others are accepted and ignored.
    static long notify(Conn* c, i32 code, const u8* payload, usize len) {
        if (code == static_cast<i32>(NOTIFY_INVAL_INODE) && len >= sizeof(InvalInodeOut)) {
            InvalInodeOut o;
            if (copy_in(&o, payload, sizeof(o)) < 0)
                return -err::FAULT;
            if (Node* n = find_node(c, o.ino))
                n->invalidate_data();
        } else if (code == static_cast<i32>(NOTIFY_INVAL_ENTRY) && len >= sizeof(InvalEntryOut)) {
            InvalEntryOut o;
            char name[256];
            if (copy_in(&o, payload, sizeof(o)) < 0 || o.namelen > 255 || len < sizeof(o) + o.namelen)
                return -err::INVAL;
            if (copy_in(name, payload + sizeof(o), o.namelen) < 0)
                return -err::FAULT;
            if (Node* n = find_node(c, o.parent))
                n->forget_name(name, o.namelen);
        }
        return 0;
    }
};

Device* g_device = nullptr;

// Every connection ever mounted (they live as long as their inodes: forever).
struct ConnLink {
    ConnLink* next;
    Conn* conn;
};
ConnLink* g_conns = nullptr;
Spinlock g_conns_lock;

u64 parse_num(const char* s, int base) {
    u64 v = 0;
    for (; *s && *s != ','; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : -1;
        if (d < 0 || d >= base)
            break;
        v = v * static_cast<u64>(base) + static_cast<u64>(d);
    }
    return v;
}

void root_attr_reply(Conn* c, Request* r) {
    auto* o = static_cast<AttrOut*>(r->out);
    if (!r->error && r->out_len >= sizeof(AttrOut) && (o->attr.mode & vfs::S_IFMT) == vfs::S_IFDIR)
        c->root->invalidate_if_needed(o->attr, expiry(o->attr_valid, o->attr_valid_nsec));
}

} // namespace

void Node::invalidate_if_needed(const Attr& a, u64 attr_expires) {
    LockGuard guard(m_lock);
    apply_attr(a, attr_expires);
    drop_cache_if_stale();
}

void create_device() {
    g_device = new Device;
    vfs::add_node("/dev/fuse", g_device);
}

long mount(const char* data, vfs::Inode** root, char* shown, usize shown_size) {
    long fd = -1;
    u32 rootmode = 0, user_id = 0, group_id = 0, max_read = 0;
    bool have_mode = false, have_uid = false, have_gid = false, allow_other = false, default_perms = false;
    for (const char* o = data ? data : ""; *o;) {
        auto opt = [&](const char* name) {
            usize n = strlen(name);
            return strncmp(o, name, n) == 0 ? o + n : nullptr;
        };
        const char* v;
        if ((v = opt("fd="))) fd = static_cast<long>(parse_num(v, 10));
        else if ((v = opt("rootmode="))) { rootmode = static_cast<u32>(parse_num(v, 8)); have_mode = true; }
        else if ((v = opt("user_id="))) { user_id = static_cast<u32>(parse_num(v, 10)); have_uid = true; }
        else if ((v = opt("group_id="))) { group_id = static_cast<u32>(parse_num(v, 10)); have_gid = true; }
        else if ((v = opt("max_read="))) max_read = static_cast<u32>(parse_num(v, 10));
        else if (opt("allow_other")) allow_other = true;
        else if (opt("default_permissions")) default_perms = true;
        while (*o && *o != ',')
            o++;
        if (*o == ',')
            o++;
    }
    if (fd < 0 || !have_mode || !have_uid || !have_gid || !vfs::is_dir(rootmode))
        return -err::INVAL;
    vfs::File* f = proc::fd_get(proc::current(), static_cast<int>(fd));
    if (!f)
        return -err::BADF;
    if (f->inode != g_device || f->private_data) {
        vfs::file_put(f);
        return -err::INVAL;
    }
    auto* c = new Conn;
    c->user_id = user_id;
    c->group_id = group_id;
    c->allow_other = allow_other;
    if (max_read >= 4096)
        c->max_read = max_read;
    c->dev = vfs::next_fs_dev();
    c->mounted = true;
    Attr a{};
    // fusermount gives only the type (040000): until the server's own
    // attributes arrive (GETATTR after INIT), the owner may enter and list it.
    a.mode = (rootmode & 07777) ? rootmode : rootmode | 0755;
    a.nlink = 2;
    a.uid = user_id;
    a.gid = group_id;
    a.ino = ROOT_ID;
    c->root = get_node(c, ROOT_ID, a, 0);
    f->private_data = c;
    vfs::file_put(f);
    {
        auto* l = new ConnLink{nullptr, c};
        LockGuard guard(g_conns_lock);
        l->next = g_conns;
        g_conns = l;
    }

    InitIn in{KERNEL_MAJOR, KERNEL_MINOR, 128 * KiB, INIT_ASYNC_READ | INIT_BIG_WRITES};
    call_background(c, OP_INIT, 0, &in, sizeof(in), sizeof(InitOut), init_reply);

    *root = c->root;
    snprintf(shown, shown_size, "rw,nosuid,nodev,relatime,user_id=%u,group_id=%u%s%s", user_id, group_id,
             default_perms ? ",default_permissions" : "", allow_other ? ",allow_other" : "");
    return 0;
}

void unmounted(vfs::Inode* root) {
    if (!root || !g_device)
        return;
    // Only the roots of our connections.
    Conn* c = nullptr;
    {
        LockGuard guard(g_conns_lock);
        for (ConnLink* l = g_conns; l; l = l->next)
            if (l->conn->mounted && l->conn->root == root) {
                c = l->conn;
                c->mounted = false;
                break;
            }
    }
    if (!c)
        return;
    abort(c);
    for_each_node(c, [](Node* x) { x->teardown(); });
}

} // namespace fuse
