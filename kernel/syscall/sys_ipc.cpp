// Shared memory and System V IPC: memfd_create, System V shared memory
// (shmget/shmat/shmdt/shmctl) and semaphores (semget/semop/semctl).
//
// All shared memory is tmpfs pages (ramfs::RegularFile): a memfd is an
// unlinked file, a System V segment is an unlinked file held by the
// segment table, POSIX shm_open() is glibc creating files in /dev/shm.
// Wayland passes memfds for window buffers; X11 MIT-SHM, ALSA dmix and
// PulseAudio use the System V calls.

#include <fs/poll.hpp>
#include <fs/ramfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>
#include <syscall/syscall.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace syscall {

namespace {

// ======================= memfd_create =======================

constexpr u32 MFD_CLOEXEC = 1, MFD_ALLOW_SEALING = 2, MFD_HUGETLB = 4, MFD_NOEXEC_SEAL = 8, MFD_EXEC = 0x10;

long sys_memfd_create(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u32 flags = static_cast<u32>(a1);
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL | MFD_EXEC))
        return flags & MFD_HUGETLB ? -err::INVAL : -err::INVAL;
    char name[250];
    if (long r = uaccess::string_from(reinterpret_cast<const char*>(a0), name, sizeof(name)); r < 0)
        return r;
    auto* file = new ramfs::RegularFile;
    file->nlink = 0; // no directory entry: freed with the last fd/mapping
    file->mode = vfs::S_IFREG | 0777;
    if (flags & (MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL))
        file->allow_sealing();
    vfs::File* f = vfs::open_inode(file, vfs::O_RDWR);
    char path[280];
    snprintf(path, sizeof(path), "/memfd:%s (deleted)", name);
    vfs::file_set_path(f, path);
    long fd = proc::fd_install(proc::current(), f, 0, flags & MFD_CLOEXEC);
    if (fd < 0)
        vfs::file_put(f);
    return fd;
}

// ======================= System V common =======================

constexpr int IPC_PRIVATE = 0;
constexpr u32 IPC_CREAT = 01000, IPC_EXCL = 02000, IPC_NOWAIT = 04000;
constexpr int IPC_RMID = 0, IPC_SET = 1, IPC_STAT = 2, IPC_INFO = 3;

// struct ipc64_perm on x86_64.
struct Ipc64Perm {
    i32 key;
    u32 uid, gid, cuid, cgid;
    u32 mode;
    u16 seq;
    u16 pad2;
    u64 unused1, unused2;
};
static_assert(sizeof(Ipc64Perm) == 48);

sched::Mutex g_ipc_lock;

// ======================= shared memory segments =======================

struct ShmidDs {
    Ipc64Perm perm;
    u64 segsz;
    i64 atime, dtime, ctime;
    i32 cpid, lpid;
    u64 nattch;
    u64 unused4, unused5;
};
static_assert(sizeof(ShmidDs) == 112);

struct Segment {
    bool used;
    i32 key;
    u64 size;
    u32 mode;
    i32 cpid, lpid;
    i64 atime, dtime, ctime;
    u16 seq;
    bool removed;              // IPC_RMID: gone from the table once nobody has it attached
    ramfs::RegularFile* file;  // the pages; kept alive by our reference and the mappings
};

constexpr int MAX_SEGMENTS = 128;
Segment g_segments[MAX_SEGMENTS];

i64 now_s() { return static_cast<i64>(rtc::realtime_ns() / 1'000'000'000); }

int shm_id(int slot) { return g_segments[slot].seq * MAX_SEGMENTS + slot; }

Segment* shm_by_id(long id) {
    if (id < 0)
        return nullptr;
    int slot = static_cast<int>(id % MAX_SEGMENTS);
    Segment& s = g_segments[slot];
    if (!s.used || s.seq != id / MAX_SEGMENTS)
        return nullptr;
    return &s;
}

// The table's own reference is a "mapping" count, so the file outlives
// IPC_RMID while any process still has the segment attached.
void shm_destroy(Segment& s) {
    ramfs::RegularFile* f = s.file;
    s.used = false;
    s.file = nullptr;
    f->mmap_ref(-1);
}

long sys_shmget(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    i32 key = static_cast<i32>(a0);
    u64 size = a1;
    u32 flags = static_cast<u32>(a2);
    LockGuard guard(g_ipc_lock);
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            Segment& s = g_segments[i];
            if (!s.used || s.removed || s.key != key)
                continue;
            if ((flags & IPC_CREAT) && (flags & IPC_EXCL))
                return -err::EXIST;
            if (size > s.size)
                return -err::INVAL;
            return shm_id(i);
        }
        if (!(flags & IPC_CREAT))
            return -err::NOENT;
    }
    if (size < 1 || size > (1ull << 32))
        return -err::INVAL;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        Segment& s = g_segments[i];
        if (s.used)
            continue;
        auto* file = new ramfs::RegularFile;
        file->nlink = 0;
        file->truncate(size);
        file->mmap_ref(+1); // the table's reference
        u16 seq = static_cast<u16>(s.seq + 1);
        s = Segment{true, key, size, flags & 0777, proc::current()->pid, 0, 0, 0, now_s(), seq, false, file};
        return shm_id(i);
    }
    return -err::NOSPC;
}

long sys_shmat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 SHM_RDONLY = 010000, SHM_RND = 020000, SHM_REMAP = 040000;
    u32 shmflg = static_cast<u32>(a2);
    uptr addr = a1;
    ramfs::RegularFile* file;
    u64 size;
    {
        LockGuard guard(g_ipc_lock);
        Segment* s = shm_by_id(static_cast<long>(a0));
        // Linux lets a segment marked for removal (IPC_RMID) still be attached
        // while it exists: X11 clients (GTK, cairo) create a segment, mark it
        // removed at once and then have the X server attach it (MIT-SHM).
        if (!s)
            return -err::INVAL;
        file = s->file;
        size = s->size;
        s->atime = now_s();
        s->lpid = proc::current()->pid;
        file->mmap_ref(+1); // hold it across map() (dropped below)
    }
    if (addr && (shmflg & SHM_RND))
        addr &= ~static_cast<uptr>(0x3FFF); // SHMLBA = 16 KiB on x86_64 (page multiple)
    u32 prot = proc::PROT_READ | ((shmflg & SHM_RDONLY) ? 0 : proc::PROT_WRITE);
    u32 flags = proc::MAP_SHARED | (addr ? ((shmflg & SHM_REMAP) ? proc::MAP_FIXED : proc::MAP_FIXED_NOREPLACE) : 0);
    uptr result;
    long r = proc::current()->mem->map(addr, size, prot, flags, file, 0, ~0ull, &result);
    file->mmap_ref(-1);
    return r < 0 ? (r == -err::EXIST ? -err::INVAL : r) : static_cast<long>(result);
}

long sys_shmdt(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    uptr addr = a0;
    if (addr & 0xFFF)
        return -err::INVAL;
    // Find which segment is mapped at addr, then unmap that whole segment.
    vfs::Inode* file = nullptr;
    usize len = 0;
    if (!proc::current()->mem->mapping_at(addr, &file, &len))
        return -err::INVAL;
    u64 seg_size = 0;
    {
        LockGuard guard(g_ipc_lock);
        for (int i = 0; i < MAX_SEGMENTS; i++) {
            if (g_segments[i].used && g_segments[i].file == file) {
                seg_size = g_segments[i].size;
                g_segments[i].dtime = now_s();
                g_segments[i].lpid = proc::current()->pid;
                break;
            }
        }
    }
    if (!seg_size)
        return -err::INVAL;
    long r = proc::current()->mem->unmap(addr, align_up<u64>(seg_size, 4096));
    // A removed segment disappears from the table with its last attachment.
    LockGuard guard(g_ipc_lock);
    for (int i = 0; i < MAX_SEGMENTS; i++) {
        Segment& s = g_segments[i];
        if (s.used && s.removed && s.file == file && s.file->mapping_count() <= 1)
            shm_destroy(s);
    }
    return r;
}

long sys_shmctl(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int cmd = static_cast<int>(a1) & ~0x100; // IPC_64 flag
    LockGuard guard(g_ipc_lock);
    if (cmd == IPC_INFO || cmd == 14 /* SHM_INFO */) {
        int highest = 0;
        for (int i = 0; i < MAX_SEGMENTS; i++)
            if (g_segments[i].used)
                highest = i;
        return highest;
    }
    Segment* s = shm_by_id(static_cast<long>(a0));
    if (!s)
        return -err::INVAL;
    switch (cmd) {
    case IPC_STAT:
    case 13: { // SHM_STAT
        ShmidDs ds{};
        ds.perm.key = s->key;
        ds.perm.mode = s->mode | (s->removed ? 01000 : 0); // SHM_DEST
        ds.perm.seq = s->seq;
        ds.segsz = s->size;
        ds.atime = s->atime;
        ds.dtime = s->dtime;
        ds.ctime = s->ctime;
        ds.cpid = s->cpid;
        ds.lpid = s->lpid;
        ds.nattch = s->file->mapping_count() - 1; // minus the table's reference (approximate: split VMAs count twice)
        return uaccess::put(reinterpret_cast<ShmidDs*>(a2), ds);
    }
    case IPC_SET: {
        ShmidDs ds;
        if (long r = uaccess::get(&ds, reinterpret_cast<ShmidDs*>(a2)); r < 0)
            return r;
        s->mode = ds.perm.mode & 0777;
        s->ctime = now_s();
        return 0;
    }
    case IPC_RMID:
        s->removed = true;
        s->key = IPC_PRIVATE; // a removed key can be created again
        if (s->file->mapping_count() <= 1)
            shm_destroy(*s);
        return 0;
    case 11: case 12: // SHM_LOCK / SHM_UNLOCK: memory is never swapped
        return 0;
    default:
        return -err::INVAL;
    }
}

// ======================= semaphores =======================

constexpr int GETPID = 11, GETVAL = 12, GETALL = 13, GETNCNT = 14, GETZCNT = 15, SETVAL = 16, SETALL = 17;
constexpr int MAX_SEMSETS = 64, MAX_SEMS = 256;

struct SemSet {
    bool used;
    i32 key;
    u32 mode;
    u16 seq;
    int nsems;
    u16* values;
    i32* pids;
    i64 otime, ctime;
};

SemSet g_sems[MAX_SEMSETS];
sched::WaitQueue g_sem_wait;       // semop waiters (re-check after every change)
volatile u64 g_sem_changes = 0;

SemSet* sem_by_id(long id) {
    if (id < 0)
        return nullptr;
    int slot = static_cast<int>(id % MAX_SEMSETS);
    SemSet& s = g_sems[slot];
    if (!s.used || s.seq != id / MAX_SEMSETS)
        return nullptr;
    return &s;
}

void sem_changed() {
    __atomic_add_fetch(&g_sem_changes, 1, __ATOMIC_SEQ_CST);
    g_sem_wait.wake_all();
}

long sys_semget(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    i32 key = static_cast<i32>(a0);
    int nsems = static_cast<int>(a1);
    u32 flags = static_cast<u32>(a2);
    LockGuard guard(g_ipc_lock);
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < MAX_SEMSETS; i++) {
            SemSet& s = g_sems[i];
            if (!s.used || s.key != key)
                continue;
            if ((flags & IPC_CREAT) && (flags & IPC_EXCL))
                return -err::EXIST;
            if (nsems > s.nsems)
                return -err::INVAL;
            return s.seq * MAX_SEMSETS + i;
        }
        if (!(flags & IPC_CREAT))
            return -err::NOENT;
    }
    if (nsems < 1 || nsems > MAX_SEMS)
        return -err::INVAL;
    for (int i = 0; i < MAX_SEMSETS; i++) {
        SemSet& s = g_sems[i];
        if (s.used)
            continue;
        u16 seq = static_cast<u16>(s.seq + 1);
        s = SemSet{true, key, flags & 0777, seq, nsems, static_cast<u16*>(kzalloc(nsems * sizeof(u16))),
                   static_cast<i32*>(kzalloc(nsems * sizeof(i32))), 0, now_s()};
        return seq * MAX_SEMSETS + i;
    }
    return -err::NOSPC;
}

struct SemBuf {
    u16 num;
    i16 op;
    i16 flg;
};

// Try to apply every operation atomically (all or nothing). 1 = done,
// 0 = would block, <0 = error. Lock held.
long sem_try(SemSet& s, const SemBuf* ops, usize n) {
    for (usize i = 0; i < n; i++)
        if (ops[i].num >= s.nsems)
            return -err::FBIG;
    i32 vals[MAX_SEMS];
    for (int i = 0; i < s.nsems; i++)
        vals[i] = s.values[i];
    for (usize i = 0; i < n; i++) {
        i32& v = vals[ops[i].num];
        if (ops[i].op == 0) {
            if (v != 0)
                return (ops[i].flg & IPC_NOWAIT) ? -err::AGAIN : 0;
        } else if (v + ops[i].op < 0) {
            return (ops[i].flg & IPC_NOWAIT) ? -err::AGAIN : 0;
        } else if (v + ops[i].op > 32767) {
            return -err::RANGE;
        } else {
            v += ops[i].op;
        }
    }
    for (int i = 0; i < s.nsems; i++)
        s.values[i] = static_cast<u16>(vals[i]);
    for (usize i = 0; i < n; i++)
        s.pids[ops[i].num] = proc::current()->pid;
    s.otime = now_s();
    return 1;
}

long semop_common(long id, u64 user_ops, u64 nops, u64 timeout_ns) {
    if (nops < 1 || nops > 500)
        return -err::INVAL;
    SemBuf ops[500];
    if (long r = uaccess::copy_from(ops, reinterpret_cast<void*>(user_ops), nops * sizeof(SemBuf)); r < 0)
        return r;
    u64 deadline = timeout_ns == ~0ull ? ~0ull : clock::now_ns() + timeout_ns;
    for (;;) {
        u64 seen = __atomic_load_n(&g_sem_changes, __ATOMIC_SEQ_CST);
        {
            LockGuard guard(g_ipc_lock);
            SemSet* s = sem_by_id(id);
            if (!s)
                return -43; // EIDRM: the set was removed
            long r = sem_try(*s, ops, nops);
            if (r != 0) {
                if (r > 0)
                    sem_changed();
                return r > 0 ? 0 : r;
            }
        }
        if (signal::interrupted())
            return -err::INTR;
        u64 now = clock::now_ns();
        if (deadline != ~0ull && now >= deadline)
            return -err::AGAIN;
        auto changed = [&] { return __atomic_load_n(&g_sem_changes, __ATOMIC_SEQ_CST) != seen || signal::interrupted(); };
        if (deadline == ~0ull)
            g_sem_wait.wait_until(changed);
        else
            g_sem_wait.wait_until_timeout(changed, deadline - now);
    }
}

long sys_semop(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return semop_common(static_cast<long>(a0), a1, a2, ~0ull);
}

long sys_semtimedop(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 timeout = ~0ull;
    if (a3) {
        i64 ts[2];
        if (long r = uaccess::copy_from(ts, reinterpret_cast<void*>(a3), sizeof(ts)); r < 0)
            return r;
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1'000'000'000)
            return -err::INVAL;
        timeout = static_cast<u64>(ts[0]) * 1'000'000'000ull + static_cast<u64>(ts[1]);
    }
    return semop_common(static_cast<long>(a0), a1, a2, timeout);
}

struct SemidDs {
    Ipc64Perm perm;
    i64 otime;
    u64 unused1;
    i64 ctime;
    u64 unused2;
    u64 nsems;
    u64 unused3, unused4;
};

long sys_semctl(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int num = static_cast<int>(a1);
    int cmd = static_cast<int>(a2) & ~0x100;
    u64 arg = a3; // union semun: an int or a pointer
    LockGuard guard(g_ipc_lock);
    if (cmd == IPC_INFO || cmd == 19 /* SEM_INFO */)
        return MAX_SEMSETS - 1;
    SemSet* s = sem_by_id(static_cast<long>(a0));
    if (!s)
        return -err::INVAL;
    switch (cmd) {
    case IPC_RMID:
        kfree(s->values);
        kfree(s->pids);
        s->used = false;
        sem_changed(); // waiters get EIDRM
        return 0;
    case IPC_STAT:
    case 18: { // SEM_STAT
        SemidDs ds{};
        ds.perm.key = s->key;
        ds.perm.mode = s->mode;
        ds.perm.seq = s->seq;
        ds.otime = s->otime;
        ds.ctime = s->ctime;
        ds.nsems = static_cast<u64>(s->nsems);
        return uaccess::put(reinterpret_cast<SemidDs*>(arg), ds);
    }
    case IPC_SET: {
        SemidDs ds;
        if (long r = uaccess::get(&ds, reinterpret_cast<SemidDs*>(arg)); r < 0)
            return r;
        s->mode = ds.perm.mode & 0777;
        s->ctime = now_s();
        return 0;
    }
    case GETVAL:
        return num < 0 || num >= s->nsems ? -err::INVAL : s->values[num];
    case GETPID:
        return num < 0 || num >= s->nsems ? -err::INVAL : s->pids[num];
    case GETNCNT: case GETZCNT:
        return 0; // waiter counts aren't tracked
    case SETVAL:
        if (num < 0 || num >= s->nsems || static_cast<int>(arg) < 0 || static_cast<int>(arg) > 32767)
            return num < 0 || num >= s->nsems ? -err::INVAL : -err::RANGE;
        s->values[num] = static_cast<u16>(arg);
        s->ctime = now_s();
        sem_changed();
        return 0;
    case GETALL:
        return uaccess::copy_to(reinterpret_cast<void*>(arg), s->values, s->nsems * sizeof(u16));
    case SETALL: {
        u16 vals[MAX_SEMS];
        if (long r = uaccess::copy_from(vals, reinterpret_cast<void*>(arg), s->nsems * sizeof(u16)); r < 0)
            return r;
        for (int i = 0; i < s->nsems; i++)
            if (vals[i] > 32767)
                return -err::RANGE;
        memcpy(s->values, vals, s->nsems * sizeof(u16));
        s->ctime = now_s();
        sem_changed();
        return 0;
    }
    default:
        return -err::INVAL;
    }
}

} // namespace

void register_ipc() {
    set(29, sys_shmget, "shmget");
    set(30, sys_shmat, "shmat");
    set(31, sys_shmctl, "shmctl");
    set(64, sys_semget, "semget");
    set(65, sys_semop, "semop");
    set(66, sys_semctl, "semctl");
    set(67, sys_shmdt, "shmdt");
    set(220, sys_semtimedop, "semtimedop");
    set(319, sys_memfd_create, "memfd_create");
}

} // namespace syscall
