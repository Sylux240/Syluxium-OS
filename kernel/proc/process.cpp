#include <arch/x86_64/compat_segments.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <arch/x86_64/protect.hpp>
#include <core/log.hpp>
#include <fs/poll.hpp>
#include <core/panic.hpp>
#include <drivers/tty.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <proc/compat.hpp>
#include <proc/elf.hpp>
#include <proc/futex.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

extern "C" [[noreturn]] void return_to_user(interrupts::Frame* frame);

namespace proc {

namespace {

constexpr u32 MSR_FS_BASE = 0xC0000100;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;

// clone() flags we understand.
constexpr u64 CLONE_VM = 0x100, CLONE_SIGHAND = 0x800,
              CLONE_VFORK = 0x4000, CLONE_PARENT = 0x8000, CLONE_THREAD = 0x10000, CLONE_SETTLS = 0x80000,
              CLONE_PARENT_SETTID = 0x100000, CLONE_CHILD_CLEARTID = 0x200000, CLONE_CHILD_SETTID = 0x1000000;

constexpr int WNOHANG = 1;

IrqSpinlock g_table_lock;
IntrusiveList<Process, &Process::table_node> g_processes;
int g_next_pid = 1;

struct UserStart {
    interrupts::Frame frame;
};

// First code of a new user thread (in the kernel): load the prepared
// register frame and drop to ring 3.
void user_thread_entry(void* arg) {
    auto* start = static_cast<UserStart*>(arg);
    interrupts::Frame frame = start->frame;
    delete start;
    return_to_user(&frame);
}

interrupts::Frame initial_frame(u64 entry, u64 stack_ptr) {
    interrupts::Frame f{};
    f.rip = entry;
    f.cs = gdt::USER_CODE;
    f.rflags = 0x202; // IF set, reserved bit 1
    f.rsp = stack_ptr;
    f.ss = gdt::USER_DATA;
    return f;
}

const char* basename(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++)
        if (*p == '/')
            b = p + 1;
    return b;
}

void close_all_files(Process* p, bool only_cloexec) {
    vfs::File* to_close[64];
    for (int start = 0; start < MAX_FDS; start += 64) {
        int n = 0;
        {
            LockGuard guard(p->fd_lock);
            for (int fd = start; fd < start + 64; fd++) {
                if (p->fds[fd] && (!only_cloexec || p->cloexec[fd])) {
                    to_close[n++] = p->fds[fd];
                    p->fds[fd] = nullptr;
                    p->cloexec[fd] = false;
                }
            }
        }
        for (int i = 0; i < n; i++)
            vfs::file_put(to_close[i]); // may wake pipe peers: outside the fd lock
    }
}

int allocate_pid() { return __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_RELAXED); }

// Create the thread that runs a process's user code.
sched::Thread* make_user_thread(Process* p, const interrupts::Frame& frame, int tid, int vtid = 0) {
    auto* start = new UserStart{frame};
    sched::Thread* t = sched::create(p->name, user_thread_entry, start, true);
    t->process = p;
    t->aspace = p->mem->aspace();
    t->fpu_state = fpu::alloc_state();
    t->user_tid = tid;
    t->vtid = vtid ? vtid : tid;
    {
        LockGuard guard(p->thread_lock);
        p->threads.push_back(t);
        p->live_threads++;
    }
    if (!p->main_thread)
        p->main_thread = t;
    return t;
}

// The signal a CPU exception in user mode raises, with siginfo's si_code and
// si_addr as Linux fills them (Wine, JITs and debuggers look at them).
struct FaultInfo {
    int sig;
    i32 code;
    bool addr_is_ip; // si_addr = the faulting instruction (else 0)
};

FaultInfo fault_info_for(u64 vector) {
    constexpr i32 FPE_INTDIV = 1, ILL_ILLOPN = 2, BUS_ADRALN = 1, TRAP_BRKPT = 1;
    switch (vector) {
    case interrupts::VEC_DIVIDE_ERROR:   return {signal::SIGFPE, FPE_INTDIV, true};
    case interrupts::VEC_DEBUG:          return {signal::SIGTRAP, TRAP_BRKPT, false};
    case interrupts::VEC_BREAKPOINT:     return {signal::SIGTRAP, signal::SI_KERNEL, false};
    case interrupts::VEC_INVALID_OPCODE: return {signal::SIGILL, ILL_ILLOPN, true};
    case 16: case 19:                    return {signal::SIGFPE, 0, true};
    case 17:                             return {signal::SIGBUS, BUS_ADRALN, false};
    default:                             return {signal::SIGSEGV, signal::SI_KERNEL, false}; // #GP & co.
    }
}

// A CPU exception in user mode: if the program installed a handler for the
// signal (and doesn't block it), queue it for this thread - it runs on the
// way back to user mode, like on Linux (JITs, Wine and garbage collectors
// rely on catching SIGSEGV). Otherwise the process dies.
void fault_signal(const char* what, u64 address, u64 ip, int sig, i32 code, u64 si_addr) {
    Process* p = current();
    sched::Thread* t = sched::current();
    if (p->sigactions[sig].handler > 1 && !(t->sigmask & (1ull << (sig - 1)))) {
        t->fault_sig = sig;
        t->fault_code = code;
        t->fault_addr = si_addr;
        signal::send_thread(t, sig);
        return;
    }
    kill_current_on_fault(what, address, ip, sig);
}

void user_fault(interrupts::Frame* frame) {
    if (!current())
        return; // not a process: let the kernel report it
    arch::sti();
    FaultInfo fi = fault_info_for(frame->vector);
    fault_signal(interrupts::exception_name(frame->vector),
                 frame->vector == interrupts::VEC_PAGE_FAULT ? arch::read_cr2() : 0, frame->rip,
                 fi.sig, fi.code, fi.addr_is_ip ? frame->rip : 0);
}

// Who called the code that touched user memory: a leaf function (memcpy)
// has its return address on top of the stack, others at rbp + 8.
u64 smap_caller(const interrupts::Frame* frame) {
    constexpr u64 TEXT = 0xffffffff80000000ull;
    u64 top = *reinterpret_cast<const u64*>(frame->rsp);
    if (top >= TEXT)
        return top;
    if (frame->rbp >= mm::KERNEL_HALF && !(frame->rbp & 7))
        return reinterpret_cast<const u64*>(frame->rbp)[1];
    return 0;
}

void page_fault(interrupts::Frame* frame) {
    constexpr i32 SEGV_MAPERR = 1, SEGV_ACCERR = 2;
    u64 addr = arch::read_cr2();
    sched::current()->last_fault_code = frame->error_code;
    bool write = frame->error_code & 2;
    bool from_user = frame->cs & 3;
    Process* p = current();
    if (from_user)
        sched::current()->fault_cr2 = addr;

    // SMAP: kernel code reading or writing a present user page outside a
    // uaccess window (reported, then allowed; panics with smap=strict).
    if (!from_user && addr < mm::USER_END && (frame->error_code & 1) && !(frame->error_code & 16) &&
        protect::smap_violation(addr, frame->rip, &frame->rflags, smap_caller(frame)))
        return;
    // The futex code reads user memory under spinlocks: it gets -EFAULT and
    // faults the page in itself after letting go (resolving here may sleep).
    if (!from_user && futex::fault_fixup(frame))
        return;
    if (p && p->mem && addr < mm::USER_END) {
        // Resolving may sleep on the memory lock; fine with interrupts on.
        if (frame->rflags & arch::RFLAGS_IF)
            arch::sti();
        if (p->mem->fault(addr, write, frame->error_code & 1))
            return;
        if (from_user) {
            arch::sti();
            bool prot = frame->error_code & 1;
            fault_signal(prot ? "protection fault" : "segmentation fault", addr, frame->rip,
                         signal::SIGSEGV, prot ? SEGV_ACCERR : SEGV_MAPERR, addr);
            return;
        }
    }
    if (from_user && p) {
        arch::sti();
        fault_signal("segmentation fault", addr, frame->rip, signal::SIGSEGV, SEGV_MAPERR, addr);
        return;
    }
    interrupts::fatal_exception(frame);
}

// Pending signals are handled every time a thread returns to user mode.
void on_return_to_user(interrupts::Frame* frame) {
    if (signal::pending(current())) {
        arch::sti();
        signal::deliver(frame);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void init() {
    vfs::set_permission_hook([](vfs::Inode* n, int mask) { return permission(n, mask); });
    vfs::set_process_root_hook([]() -> vfs::Inode* {
        Process* p = current();
        return p ? p->root : nullptr;
    });
    vfs::set_process_mounts_hook([]() -> void* {
        Process* p = current();
        return p ? p->mntns : nullptr;
    });
    interrupts::set_handler(interrupts::VEC_PAGE_FAULT, page_fault);
    interrupts::set_user_fault_hook(user_fault);
    interrupts::set_return_to_user_hook(on_return_to_user);
}

IrqSpinlock& table_lock() { return g_table_lock; }

Process* find(int pid) {
    for (Process* p : g_processes)
        if (p->pid == pid)
            return p;
    return nullptr;
}

void for_each(void (*fn)(Process*, void*), void* ctx) {
    LockGuard guard(g_table_lock);
    for (Process* p : g_processes)
        if (p->state == State::Running)
            fn(p, ctx);
}

u32 count() {
    LockGuard guard(g_table_lock);
    return static_cast<u32>(g_processes.size());
}

Process* current() { return sched::current()->process; }

// ---- credentials and permissions ----

namespace {
const Cred g_kernel_cred; // kernel threads act as root
}

const Cred& cred() {
    Process* p = current();
    return p ? p->cred : g_kernel_cred;
}

bool capable() { return cred().euid == 0; }

bool in_group(u32 gid) {
    const Cred& c = cred();
    if (c.fsgid == gid)
        return true;
    for (u32 i = 0; i < c.ngroups; i++)
        if (c.groups[i] == gid)
            return true;
    return false;
}

long permission(vfs::Inode* n, int mask, bool real_ids) {
    const Cred& c = cred();
    u32 uid = real_ids ? c.ruid : c.fsuid;
    if (uid == 0) { // root reads and writes anything; executes what has some x bit
        if ((mask & MAY_EXEC) && !vfs::is_dir(n->mode) && !(n->mode & 0111))
            return -err::ACCES;
        return 0;
    }
    u32 bits;
    if (n->uid == uid)
        bits = n->mode >> 6;
    else if ((real_ids ? c.rgid == n->gid : false) || in_group(n->gid))
        bits = n->mode >> 3;
    else
        bits = n->mode;
    return ((bits & 7) & static_cast<u32>(mask)) == static_cast<u32>(mask) ? 0 : -err::ACCES;
}

bool owns(vfs::Inode* n) {
    const Cred& c = cred();
    return c.fsuid == 0 || c.fsuid == n->uid;
}

long may_delete(vfs::Inode* dir, vfs::Inode* victim) {
    if (long r = permission(dir, MAY_WRITE | MAY_EXEC); r < 0)
        return r;
    const Cred& c = cred();
    if ((dir->mode & vfs::S_ISVTX) && c.fsuid != 0 && victim && c.fsuid != victim->uid && c.fsuid != dir->uid)
        return -err::PERM;
    return 0;
}

void init_owner(vfs::Inode* dir, vfs::Inode* n) {
    const Cred& c = cred();
    u32 gid = c.fsgid;
    u32 mode = n->mode;
    if (dir && (dir->mode & vfs::S_ISGID)) {
        gid = dir->gid; // BSD-style group inheritance in setgid directories
        if (vfs::is_dir(n->mode))
            mode |= vfs::S_ISGID;
    }
    if (n->uid != c.fsuid || n->gid != gid || n->mode != mode)
        n->set_owner(mode, c.fsuid, gid);
}

// ---- File descriptors ------------------------------------------------------

long fd_install(Process* p, vfs::File* f, int min_fd, bool cloexec) {
    LockGuard guard(p->fd_lock);
    for (int fd = min_fd; fd < MAX_FDS; fd++) {
        if (!p->fds[fd]) {
            p->fds[fd] = f;
            p->cloexec[fd] = cloexec;
            return fd;
        }
    }
    return -err::MFILE;
}

vfs::File* fd_get(Process* p, int fd) {
    if (fd < 0 || fd >= MAX_FDS)
        return nullptr;
    LockGuard guard(p->fd_lock);
    vfs::File* f = p->fds[fd];
    if (f)
        vfs::file_get(f);
    return f;
}

long fd_replace(Process* p, int fd, vfs::File* f) {
    if (fd < 0 || fd >= MAX_FDS)
        return -err::BADF;
    vfs::File* old;
    {
        LockGuard guard(p->fd_lock);
        old = p->fds[fd];
        p->fds[fd] = f;
    }
    if (old)
        vfs::file_put(old);
    return 0;
}

long fd_close(Process* p, int fd) {
    if (fd < 0 || fd >= MAX_FDS)
        return -err::BADF;
    vfs::File* f;
    {
        LockGuard guard(p->fd_lock);
        f = p->fds[fd];
        if (!f)
            return -err::BADF;
        p->fds[fd] = nullptr;
    }
    vfs::file_put(f);
    return 0;
}

long fd_dup2(Process* p, int old_fd, int new_fd, bool cloexec) {
    if (old_fd < 0 || old_fd >= MAX_FDS || new_fd < 0 || new_fd >= MAX_FDS)
        return -err::BADF;
    vfs::File* replaced = nullptr;
    {
        LockGuard guard(p->fd_lock);
        vfs::File* f = p->fds[old_fd];
        if (!f)
            return -err::BADF;
        if (old_fd == new_fd)
            return new_fd;
        vfs::file_get(f);
        replaced = p->fds[new_fd];
        p->fds[new_fd] = f;
        p->cloexec[new_fd] = cloexec;
    }
    if (replaced)
        vfs::file_put(replaced);
    return new_fd;
}

// ---- Kernel API: start a program ---------------------------------------------

long spawn(const char* path, const char* const* argv, const char* const* envp, Process** out) {
    vfs::Inode* inode;
    long r = vfs::resolve(path, nullptr, true, &inode);
    if (r < 0)
        return r;
    if (!vfs::is_reg(inode->mode))
        return -err::ACCES;

    auto* as = vmm::AddressSpace::create_user();
    if (!as)
        return -err::NOMEM;
    auto* p = new Process;
    p->mem = new Memory(as);

    elf::LoadResult loaded;
    r = elf::load(p->mem, inode, path, argv, envp, &loaded);
    if (r < 0) {
        p->mem->put();
        delete p;
        return r;
    }

    snprintf(p->name, sizeof(p->name), "%s", basename(path));
    snprintf(p->exe_path, sizeof(p->exe_path), "%s", path);
    p->cwd = vfs::root();
    p->kernel_waited = true;

    // stdin, stdout, stderr: the console.
    vfs::Inode* console;
    if (vfs::resolve("/dev/console", nullptr, true, &console) == 0) {
        vfs::File* f = vfs::open_inode(console, vfs::O_RDWR);
        vfs::file_set_path(f, "/dev/console");
        fd_install(p, f);
        fd_dup2(p, 0, 1, false);
        fd_dup2(p, 0, 2, false);
    }

    {
        LockGuard guard(g_table_lock);
        p->pid = allocate_pid();
        p->vpid = p->pid;
        p->pgid = p->sid = p->pid; // its own session and process group
        g_processes.push_back(p);
    }

    p->start_ns = clock::now_ns();
    p->compat = loaded.compat;
    sched::Thread* t = make_user_thread(p, loaded.compat ? compat::initial_frame(loaded.entry, loaded.stack_ptr)
                                                         : initial_frame(loaded.entry, loaded.stack_ptr),
                                        p->pid);
    sched::start(t);
    *out = p;
    return 0;
}

int wait(Process* p) {
    p->exit_done.wait();
    int status = p->exit_status;
    {
        LockGuard guard(g_table_lock);
        g_processes.remove(p);
    }
    delete p;
    return status;
}

// ---- fork / clone ----------------------------------------------------------

namespace {

// clone(CLONE_THREAD): a new thread in the calling process (pthread_create).
long clone_thread(interrupts::Frame* frame, u64 flags, u64 new_sp, u64 parent_tid, u64 child_tid, u64 tls) {
    if (!(flags & CLONE_VM) || !(flags & CLONE_SIGHAND))
        return -err::INVAL; // Linux requires both for threads
    Process* p = current();
    sched::Thread* me = sched::current();
    if (p->group_exiting)
        return -err::AGAIN;

    int tid = allocate_pid();
    int vtid = tid;
    if (p->pidns) {
        LockGuard guard(g_table_lock);
        vtid = p->pidns->next++;
    }
    interrupts::Frame cf = *frame;
    cf.rax = 0;
    if (new_sp)
        cf.rsp = new_sp;
    sched::Thread* t = make_user_thread(p, cf, tid, vtid);
    fpu::save(me->fpu_state);
    memcpy(t->fpu_state, me->fpu_state, fpu::state_size());
    t->fs_base = (flags & CLONE_SETTLS) ? tls : me->fs_base;
    t->gs_base = arch::rdmsr(MSR_KERNEL_GS_BASE);
    // TLS descriptors/selectors are inherited (64-bit programs have them too:
    // Wine). For 32-bit programs CLONE_SETTLS's tls points at a user_desc.
    compat::clone_tls(t, me, p->compat && (flags & CLONE_SETTLS), tls);
    if (!p->compat && (flags & CLONE_SETTLS))
        t->user_fs_sel = 0; // 64-bit: the new base wins over an inherited selector
    t->sigmask = me->sigmask;
    snprintf(t->name, sizeof(t->name), "%s", p->name);

    // Shared memory: the tid words are written directly in our address space.
    if (flags & CLONE_PARENT_SETTID)
        uaccess::put(reinterpret_cast<i32*>(parent_tid), vtid);
    if (flags & CLONE_CHILD_SETTID)
        uaccess::put(reinterpret_cast<i32*>(child_tid), vtid);
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = child_tid;

    sched::start(t);
    return vtid;
}

} // namespace

long do_clone(interrupts::Frame* frame, u64 flags, u64 new_sp, u64 parent_tid, u64 child_tid, u64 tls) {
    constexpr u64 CLONE_NEWPID = 0x20000000;
    if (flags & CLONE_THREAD)
        return clone_thread(frame, flags, new_sp, parent_tid, child_tid, tls);
    Process* parent = current();
    if (flags & CLONE_NEWPID) {
        if (!capable())
            return -err::PERM;
        if (parent->pidns)
            return -err::INVAL; // one level of PID namespaces
    }
    sched::Thread* parent_thread = sched::current();

    auto* child = new Process;
    if (flags & CLONE_VM) {
        child->mem = parent->mem; // vfork-style: share until the child execs or exits
        child->mem->get();
    } else {
        child->mem = parent->mem->fork();
        if (!child->mem) {
            delete child;
            return -err::NOMEM;
        }
    }

    memcpy(child->name, parent->name, sizeof(child->name));
    memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
    child->exe_inode = parent->exe_inode;
    memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
    child->cwd = parent->cwd;
    child->root = parent->root;
    if (flags & 0x20000 /* CLONE_NEWNS */) {
        child->mntns = vfs::mount_ns_copy(parent->mntns);
    } else {
        child->mntns = parent->mntns;
        vfs::mount_ns_get(child->mntns);
    }
    if (flags & 0x04000000 /* CLONE_NEWUTS */) {
        child->uts = new Uts(*(parent->uts ? parent->uts : system_uts()));
        child->uts->refs = 1;
    } else if (parent->uts) {
        child->uts = parent->uts;
        uts_get(child->uts);
    }
    child->umask = parent->umask;
    child->personality = parent->personality;
    child->cred = parent->cred;
    child->oom_score_adj = parent->oom_score_adj;
    child->ctty = parent->ctty;
    for (int i = 0; i <= MAX_SIGNALS; i++)
        child->sigactions[i] = parent->sigactions[i];
    child->exit_signal = static_cast<int>(flags & 0xFF);
    child->compat = parent->compat;
    child->vfork_child = flags & CLONE_VFORK;

    {
        LockGuard guard(parent->fd_lock);
        for (int fd = 0; fd < MAX_FDS; fd++) {
            if (parent->fds[fd]) {
                vfs::file_get(parent->fds[fd]);
                child->fds[fd] = parent->fds[fd];
                child->cloexec[fd] = parent->cloexec[fd];
            }
        }
    }

    {
        LockGuard guard(g_table_lock);
        child->pid = allocate_pid();
        child->vpid = child->pid;
        if (flags & CLONE_NEWPID) {
            child->pidns = new PidNs;
            child->pidns->init = child;
            child->vpid = child->pidns->next++;
        } else if (parent->pidns || parent->child_pidns) {
            child->pidns = parent->pidns ? parent->pidns : parent->child_pidns;
            __atomic_add_fetch(&child->pidns->refs, 1, __ATOMIC_RELAXED);
            child->vpid = child->pidns->next++;
        }
        child->pgid = parent->pgid;
        child->sid = parent->sid;
        child->parent = (flags & CLONE_PARENT) && parent->parent ? parent->parent : parent;
        child->parent->children.push_back(child);
        g_processes.push_back(child);
    }
    int pid = child->pid;
    int vpid = child->vpid;              // what the child calls itself
    int parent_view = pid_nr(child, parent); // what the parent calls it

    // The child resumes from the same syscall, seeing 0 as the result.
    interrupts::Frame cf = *frame;
    cf.rax = 0;
    if (new_sp)
        cf.rsp = new_sp;
    child->start_ns = clock::now_ns();
    sched::Thread* t = make_user_thread(child, cf, pid, vpid);
    t->sigmask = parent_thread->sigmask;
    fpu::save(parent_thread->fpu_state);
    memcpy(t->fpu_state, parent_thread->fpu_state, fpu::state_size());
    t->fs_base = (flags & CLONE_SETTLS) ? tls : parent_thread->fs_base;
    t->gs_base = arch::rdmsr(MSR_KERNEL_GS_BASE);
    compat::clone_tls(t, parent_thread, child->compat && (flags & CLONE_SETTLS), tls);
    if (!child->compat && (flags & CLONE_SETTLS))
        t->user_fs_sel = 0;

    if (flags & CLONE_PARENT_SETTID)
        uaccess::put(reinterpret_cast<i32*>(parent_tid), parent_view);
    if (flags & CLONE_CHILD_SETTID)
        child->mem->write_remote(child_tid, &vpid, sizeof(i32));
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = child_tid;

    sched::start(t);

    if (flags & CLONE_VFORK)
        child->vfork_done.wait(); // the child can't be reaped before we return: we're its parent
    return parent_view;
}

// ---- execve ----------------------------------------------------------------

namespace {

long exec_impl(interrupts::Frame* frame, const char* path, const char* const* argv, const char* const* envp,
               int depth, vfs::Inode* given = nullptr) {
    Process* p = current();
    vfs::Inode* inode = given;
    if (!inode) {
        if (long r = vfs::resolve(path, p->cwd, true, &inode); r < 0)
            return r;
    }
    if (!vfs::is_reg(inode->mode))
        return -err::ACCES;
    if (long r = permission(inode, MAY_EXEC); r < 0)
        return r;

    // "#!interpreter [arg]" scripts.
    char head[256];
    isize n = inode->read(0, head, sizeof(head) - 1);
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        if (depth >= 4)
            return -err::LOOP;
        head[n] = 0;
        char* line = head + 2;
        while (*line == ' ' || *line == '\t') line++;
        char* end = line;
        while (*end && *end != '\n') end++;
        *end = 0;
        char* interp = line;
        char* arg = interp;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        if (*arg) {
            *arg++ = 0;
            while (*arg == ' ' || *arg == '\t') arg++;
            for (char* t = arg + strlen(arg); t > arg && (t[-1] == ' ' || t[-1] == '\t');) *--t = 0;
        }
        if (!*interp)
            return -err::NOEXEC;

        // New argv: interpreter [arg] script original-args...
        usize argc = 0;
        while (argv[argc]) argc++;
        auto** new_argv = static_cast<const char**>(kmalloc((argc + 3) * sizeof(char*)));
        if (!new_argv)
            return -err::NOMEM;
        usize i = 0;
        new_argv[i++] = interp;
        if (*arg)
            new_argv[i++] = arg;
        new_argv[i++] = path;
        for (usize k = 1; k < argc; k++)
            new_argv[i++] = argv[k];
        new_argv[i] = nullptr;
        long r = exec_impl(frame, interp, new_argv, envp, depth + 1);
        kfree(new_argv);
        return r;
    }

    // setuid / setgid programs run with their owner's / group's identity
    // (not for scripts: the "#!" path above already recursed). The saved ids
    // follow the effective ones, as POSIX says.
    Cred next = p->cred;
    if (inode->mode & vfs::S_ISUID)
        next.euid = inode->uid;
    if ((inode->mode & vfs::S_ISGID) && (inode->mode & 0010))
        next.egid = inode->gid;
    next.suid = next.fsuid = next.euid;
    next.sgid = next.fsgid = next.egid;
    elf::ExecIds ids{next.ruid, next.euid, next.rgid, next.egid,
                     next.euid != next.ruid || next.egid != next.rgid};

    auto* as = vmm::AddressSpace::create_user();
    if (!as)
        return -err::NOMEM;
    auto* mem = new Memory(as);
    elf::LoadResult loaded;
    if (long r = elf::load(mem, inode, path, argv, envp, &loaded, ids); r < 0) {
        mem->put(); // not active: safe to tear down
        return r;
    }

    // ---- Point of no return: the old program is gone after this. ----

    // The new program starts with only the calling thread: stop all others
    // and wait until they've left (they exit on their way back to user mode).
    sched::Thread* self = sched::current();
    if (p->live_threads > 1) {
        p->zap_except = self;
        signal::wake_all_threads(p);
        p->thread_exit_wait.wait_until([&] { return p->live_threads == 1; });
        p->zap_except = nullptr;
    }
    close_all_files(p, true);
    for (int sig = 1; sig <= MAX_SIGNALS; sig++) {
        if (p->sigactions[sig].handler > 1) // handlers live in the old image; SIG_IGN survives exec
            p->sigactions[sig] = SigAction{};
    }

    sched::Thread* t = sched::current();
    Memory* old;
    {
        LockGuard guard(table_lock()); // as in finish_process: /proc takes references under it
        old = p->mem;
        p->mem = mem;
    }
    {
        InterruptGuard guard; // the address space switch outside the table lock
        t->aspace = as;
        as->activate();
    }
    old->put(); // a vfork parent may still hold it

    p->cred = next;
    snprintf(p->name, sizeof(p->name), "%s", basename(path));
    // /proc/self/exe is the real file (symlinks resolved), as on Linux:
    // programs find their own install directory through it.
    p->exe_inode = inode;
    if (vfs::canonical_path(path, p->cwd_path, p->exe_path, sizeof(p->exe_path)) < 0)
        snprintf(p->exe_path, sizeof(p->exe_path), "%s", path);
    snprintf(t->name, sizeof(t->name), "%s", p->name);
    p->compat = loaded.compat;
    compat_segments::reset(t, p->compat); // drop the old program's TLS selectors/descriptors
    t->fs_base = 0;
    t->gs_base = 0;
    arch::wrmsr(MSR_FS_BASE, 0);
    arch::wrmsr(MSR_KERNEL_GS_BASE, 0);
    fpu::reset(t->fpu_state);
    fpu::restore(t->fpu_state);

    *frame = loaded.compat ? compat::initial_frame(loaded.entry, loaded.stack_ptr)
                           : initial_frame(loaded.entry, loaded.stack_ptr);
    frame->vector = 0x80;

    if (p->vfork_child) {
        p->vfork_child = false;
        p->vfork_done.complete_all();
    }
    return 0;
}

} // namespace

long do_execve(interrupts::Frame* frame, const char* path, const char* const* argv, const char* const* envp) {
    return exec_impl(frame, path, argv, envp, 0);
}

long do_execve_inode(interrupts::Frame* frame, vfs::Inode* inode, const char* path, const char* const* argv,
                     const char* const* envp) {
    return exec_impl(frame, path, argv, envp, 0, inode);
}

// ---- wait4 -----------------------------------------------------------------

int pid_nr_locked(const Process* p, const Process* viewer);
int pgid_global_locked(int pgid, const Process* viewer);

long do_wait4(int pid, int* status_out, int options, u32* uid_out) {
    constexpr int WNOWAIT = 0x01000000; // report, but leave the child to be waited for again
    Process* me = current();
    for (;;) {
        u64 seen;
        {
            LockGuard guard(me->child_wait.lock());
            seen = me->child_events;
        }

        Process* reaped = nullptr;
        bool any = false;
        {
            LockGuard guard(g_table_lock);
            int want_pgid = pid < -1 ? pgid_global_locked(-pid, me) : 0;
            for (Process* c : me->children) {
                bool match = pid > 0 ? pid_nr_locked(c, me) == pid
                           : pid == -1 ? true
                           : pid == 0 ? c->pgid == me->pgid
                                      : c->pgid == want_pgid;
                if (!match)
                    continue;
                any = true;
                if (c->state == State::Zombie) {
                    reaped = c;
                    if (!(options & WNOWAIT)) {
                        me->children.remove(c);
                        g_processes.remove(c);
                    }
                    break;
                }
            }
        }

        if (reaped) {
            int child_pid;
            {
                LockGuard guard(g_table_lock);
                child_pid = pid_nr_locked(reaped, me);
            }
            *status_out = reaped->exit_status;
            if (uid_out)
                *uid_out = reaped->cred.ruid;
            if (!(options & WNOWAIT))
                delete reaped;
            return child_pid;
        }
        if (!any)
            return -err::CHILD;
        if (options & WNOHANG)
            return 0;
        if (signal::interrupted())
            return -err::INTR;
        me->child_wait.wait_until([&] { return me->child_events != seen || signal::interrupted(); });
    }
}

// ---- exit ------------------------------------------------------------------

namespace {

// Last thread gone: tear the process down and become a zombie.
[[noreturn]] void finish_process(Process* p, sched::Thread* me, int wait_status) {
    close_all_files(p, false);

    // Leave the address space before dropping it.
    {
        InterruptGuard guard;
        me->aspace = nullptr;
        vmm::AddressSpace::kernel().activate();
    }
    // Detach the memory under the table lock before dropping it: /proc
    // readers take their own reference under that lock, and must never
    // find a pointer to memory that is already freed.
    Memory* mem;
    {
        LockGuard guard(table_lock());
        mem = p->mem;
        p->mem = nullptr;
    }
    mem->put();
    me->process = nullptr; // from here on we're an ordinary kernel thread

    if (p->vfork_child)
        p->vfork_done.complete_all();

    Process* orphan_zombies[32];
    int orphan_count = 0;
    bool free_self = false;
    {
        LockGuard guard(g_table_lock);
        // In a PID namespace its init adopts our children (and reaps them);
        // the init's own death ends the namespace: everyone in it is killed.
        PidNs* ns = p->pidns;
        Process* adopter = ns && ns->init && ns->init != p && ns->init->state != State::Zombie ? ns->init : nullptr;
        if (ns && ns->init == p) {
            ns->init = nullptr;
            for (Process* q : g_processes)
                if (q->pidns == ns && q != p && q->state != State::Zombie)
                    signal::send(q, 9 /* SIGKILL */);
        }
        bool adopted_zombie = false;
        // Our children lose their parent. Zombies among them are freed now;
        // living ones free themselves when they exit.
        while (Process* c = p->children.pop_front()) {
            if (adopter) {
                c->parent = adopter;
                adopter->children.push_back(c);
                adopted_zombie |= c->state == State::Zombie;
                continue;
            }
            c->parent = nullptr;
            if (c->state == State::Zombie && orphan_count < 32) {
                g_processes.remove(c);
                orphan_zombies[orphan_count++] = c;
            }
        }
        if (adopted_zombie) {
            {
                LockGuard wq_guard(adopter->child_wait.lock());
                adopter->child_events++;
            }
            adopter->child_wait.wake_all();
            signal::send(adopter, 17 /* SIGCHLD */);
        }
        p->exit_status = wait_status;
        p->state = State::Zombie;
        poll::notify(); // pidfds of this process become readable

        if (Process* parent = p->parent) {
            {
                LockGuard wq_guard(parent->child_wait.lock());
                parent->child_events++;
            }
            parent->child_wait.wake_all();
            signal::send(parent, p->exit_signal);
        } else if (!p->kernel_waited) {
            g_processes.remove(p); // nobody will wait for us
            free_self = true;
        }
    }
    for (int i = 0; i < orphan_count; i++)
        delete orphan_zombies[i];

    if (free_self)
        delete p;
    else if (p->kernel_waited)
        p->exit_done.complete_all(); // after this, `p` may be freed at any moment
    sched::exit(wait_status);
}

} // namespace

void exit_thread(int code) {
    Process* p = current();
    KASSERT(p != nullptr);
    sched::Thread* me = sched::current();

    // Robust mutexes it still holds: marked "owner died", a waiter woken.
    if (me->robust_list && p->mem) {
        futex::exit_robust_list(me->robust_list, me->vtid);
        me->robust_list = 0;
    }
    // pthread_join waits on this word: clear it and wake the joiner.
    if (me->clear_child_tid && p->mem) {
        i32 zero = 0;
        if (uaccess::put(reinterpret_cast<i32*>(me->clear_child_tid), zero) == 0)
            futex::wake(me->clear_child_tid, 1, ~0u, false); // shared, as on Linux (glibc waits that way)
        me->clear_child_tid = 0;
    }

    u32 remaining;
    {
        LockGuard guard(p->thread_lock);
        p->threads.remove(me);
        remaining = --p->live_threads;
        p->dead_utime += me->utime_ticks;
        p->dead_stime += me->stime_ticks;
        if (p->main_thread == me)
            p->main_thread = p->threads.front();
    }
    p->thread_exit_wait.wake_all();

    if (remaining > 0) {
        // Other threads carry on: just leave.
        {
            InterruptGuard guard;
            me->aspace = nullptr;
            vmm::AddressSpace::kernel().activate();
        }
        me->process = nullptr;
        sched::exit(code);
    }
    int status = p->group_exiting ? p->group_exit_status : (code & 0xFF) << 8;
    finish_process(p, me, status);
}

void exit_group(int wait_status) {
    Process* p = current();
    KASSERT(p != nullptr);
    if (!__atomic_exchange_n(&p->group_exiting, true, __ATOMIC_ACQ_REL))
        p->group_exit_status = wait_status;
    signal::wake_all_threads(p); // the others exit on their way back to user mode
    exit_thread(0);
}

void exit_current(int wait_status) { exit_group(wait_status); }

// ---- PID namespaces ----

int pid_nr_locked(const Process* p, const Process* viewer) {
    if (!viewer || !viewer->pidns)
        return p->pid;
    return p->pidns == viewer->pidns ? p->vpid : 0;
}

int pid_nr(const Process* p, const Process* viewer) {
    LockGuard guard(g_table_lock);
    return pid_nr_locked(p, viewer);
}

bool visible(const Process* p, const Process* viewer) {
    return !viewer || !viewer->pidns || p->pidns == viewer->pidns;
}

Process* find_visible(int pid, const Process* viewer) {
    if (!viewer || !viewer->pidns)
        return find(pid);
    for (Process* p : g_processes)
        if (p->pidns == viewer->pidns && p->vpid == pid)
            return p;
    return nullptr;
}

int pgid_nr(int global_pgid, const Process* viewer) {
    if (!viewer || !viewer->pidns)
        return global_pgid;
    LockGuard guard(g_table_lock);
    Process* leader = find(global_pgid);
    return leader && leader->pidns == viewer->pidns ? leader->vpid : 0;
}

int pgid_global_locked(int pgid, const Process* viewer) {
    if (!viewer || !viewer->pidns)
        return pgid;
    Process* leader = find_visible(pgid, viewer);
    return leader ? leader->pid : -1;
}

int pgid_global(int pgid, const Process* viewer) {
    LockGuard guard(g_table_lock);
    return pgid_global_locked(pgid, viewer);
}

int tid_nr(const sched::Thread* t, const Process* viewer) {
    if (!viewer || !viewer->pidns)
        return t->user_tid;
    return t->process && t->process->pidns == viewer->pidns ? t->vtid : 0;
}

sched::Thread* find_visible_thread(int tid, const Process* viewer) {
    for (Process* p : g_processes) {
        if (!visible(p, viewer))
            continue;
        LockGuard guard(p->thread_lock);
        for (sched::Thread* t : p->threads)
            if ((viewer && viewer->pidns ? t->vtid : t->user_tid) == tid)
                return t;
    }
    return nullptr;
}

sched::Thread* find_thread(int tid) {
    for (Process* p : g_processes) {
        LockGuard guard(p->thread_lock);
        for (sched::Thread* t : p->threads)
            if (t->user_tid == tid)
                return t;
    }
    return nullptr;
}

namespace {
// Describe where `addr` lies: in a mapping (range, permissions, file
// offset) or after the nearest one below (how far past its end).
void describe_address(Memory* mem, const char* label, u64 addr) {
    struct Area {
        uptr start = 0, end = 0;
        u32 prot = 0;
        bool file = false;
        u64 offset = 0;
        bool valid = false;
    };
    struct Ctx {
        u64 addr;
        Area in, below;
    } c{addr, {}, {}};
    mem->for_each_vma([](const Vma& v, void* a) {
        auto* ctx = static_cast<Ctx*>(a);
        Area area{v.start, v.end, v.prot, v.file != nullptr, v.file_offset, true};
        if (ctx->addr >= v.start && ctx->addr < v.end)
            ctx->in = area;
        else if (v.end <= ctx->addr && (!ctx->below.valid || v.end > ctx->below.end))
            ctx->below = area;
    }, &c);
    auto prot = [](u32 p, char* out) {
        out[0] = (p & PROT_READ) ? 'r' : '-';
        out[1] = (p & PROT_WRITE) ? 'w' : '-';
        out[2] = (p & PROT_EXEC) ? 'x' : '-';
        out[3] = 0;
    };
    char pr[4];
    if (c.in.valid) {
        prot(c.in.prot, pr);
        klog::info("  %s %#lx: in %#lx-%#lx %s %s+%#lx", label, addr, c.in.start, c.in.end, pr,
                   c.in.file ? "file" : "anon", c.in.offset + (addr - c.in.start));
    } else if (c.below.valid) {
        prot(c.below.prot, pr);
        klog::info("  %s %#lx: unmapped, %#lx bytes past %#lx-%#lx %s %s", label, addr, addr - c.below.end,
                   c.below.start, c.below.end, pr, c.below.file ? "file" : "anon");
    } else {
        klog::info("  %s %#lx: unmapped (below every mapping)", label, addr);
    }
}
} // namespace

void kill_current_on_fault(const char* what, u64 address, u64 ip, int sig) {
    Process* p = current();
    klog::error("%s[%d]: %s at %#lx, ip %#lx - killed (signal %d)", p->name, p->pid, what, address, ip, sig);
    u64 ec = sched::current()->last_fault_code;
    if (sig == signal::SIGSEGV)
        klog::info("  page fault: %s, %s, %s%s", (ec & 1) ? "page present" : "page not present",
                   (ec & 2) ? "write" : "read", (ec & 4) ? "user" : "kernel", (ec & 16) ? ", instruction fetch" : "");
    if (p->mem) {
        describe_address(p->mem, "ip", ip);
        if (address)
            describe_address(p->mem, "address", address);
    }
    exit_current(sig);
}

} // namespace proc

namespace linuxkpi {
void process_gone(proc::Process* p); // kernel/linuxkpi/binder.cpp
}
proc::Process::~Process() {
    if (lkpi_task || lkpi_mm)
        linuxkpi::process_gone(this);
    if (pidns && __atomic_sub_fetch(&pidns->refs, 1, __ATOMIC_ACQ_REL) == 0)
        delete pidns;
    if (child_pidns && __atomic_sub_fetch(&child_pidns->refs, 1, __ATOMIC_ACQ_REL) == 0)
        delete child_pidns;
    if (uts)
        proc::uts_put(uts);
    vfs::mount_ns_put(mntns);
}

namespace proc {
Uts* system_uts() {
    static Uts u;
    return &u;
}
void uts_get(Uts* u) { __atomic_add_fetch(&u->refs, 1, __ATOMIC_RELAXED); }
void uts_put(Uts* u) {
    if (u != system_uts() && __atomic_sub_fetch(&u->refs, 1, __ATOMIC_ACQ_REL) == 0)
        delete u;
}
} // namespace proc
