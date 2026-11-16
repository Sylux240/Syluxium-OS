#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <drivers/tty.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
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
sched::Thread* make_user_thread(Process* p, const interrupts::Frame& frame, int tid) {
    auto* start = new UserStart{frame};
    sched::Thread* t = sched::create(p->name, user_thread_entry, start, true);
    t->process = p;
    t->aspace = p->mem->aspace();
    t->fpu_state = fpu::alloc_state();
    t->user_tid = tid;
    {
        LockGuard guard(p->thread_lock);
        p->threads.push_back(t);
        p->live_threads++;
    }
    if (!p->main_thread)
        p->main_thread = t;
    return t;
}

int signal_for(u64 vector) {
    switch (vector) {
    case interrupts::VEC_DIVIDE_ERROR:   return signal::SIGFPE;
    case interrupts::VEC_BREAKPOINT:     return signal::SIGTRAP;
    case interrupts::VEC_INVALID_OPCODE: return signal::SIGILL;
    case 16: case 19:                    return signal::SIGFPE;
    case 17:                             return signal::SIGBUS;
    default:                             return signal::SIGSEGV;
    }
}

void user_fault(interrupts::Frame* frame) {
    if (!current())
        return; // not a process: let the kernel report it
    arch::sti();
    kill_current_on_fault(interrupts::exception_name(frame->vector),
                          frame->vector == interrupts::VEC_PAGE_FAULT ? arch::read_cr2() : 0, frame->rip,
                          signal_for(frame->vector));
}

void page_fault(interrupts::Frame* frame) {
    u64 addr = arch::read_cr2();
    bool write = frame->error_code & 2;
    bool from_user = frame->cs & 3;
    Process* p = current();

    if (p && p->mem && addr < mm::USER_END) {
        // Resolving may sleep on the memory lock; fine with interrupts on.
        if (frame->rflags & arch::RFLAGS_IF)
            arch::sti();
        if (p->mem->fault(addr, write))
            return;
        if (from_user) {
            arch::sti();
            kill_current_on_fault((frame->error_code & 1) ? "protection fault" : "segmentation fault", addr,
                                  frame->rip, signal::SIGSEGV);
        }
    }
    if (from_user && p) {
        arch::sti();
        kill_current_on_fault("segmentation fault", addr, frame->rip, signal::SIGSEGV);
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
        p->pgid = p->sid = p->pid; // its own session and process group
        g_processes.push_back(p);
    }

    p->start_ns = clock::now_ns();
    sched::Thread* t = make_user_thread(p, initial_frame(loaded.entry, loaded.stack_ptr), p->pid);
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
    interrupts::Frame cf = *frame;
    cf.rax = 0;
    if (new_sp)
        cf.rsp = new_sp;
    sched::Thread* t = make_user_thread(p, cf, tid);
    fpu::save(me->fpu_state);
    memcpy(t->fpu_state, me->fpu_state, fpu::state_size());
    t->fs_base = (flags & CLONE_SETTLS) ? tls : me->fs_base;
    t->gs_base = arch::rdmsr(MSR_KERNEL_GS_BASE);
    t->sigmask = me->sigmask;
    snprintf(t->name, sizeof(t->name), "%s", p->name);

    // Shared memory: the tid words are written directly in our address space.
    if (flags & CLONE_PARENT_SETTID)
        uaccess::put(reinterpret_cast<i32*>(parent_tid), tid);
    if (flags & CLONE_CHILD_SETTID)
        uaccess::put(reinterpret_cast<i32*>(child_tid), tid);
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = child_tid;

    sched::start(t);
    return tid;
}

} // namespace

long do_clone(interrupts::Frame* frame, u64 flags, u64 new_sp, u64 parent_tid, u64 child_tid, u64 tls) {
    if (flags & CLONE_THREAD)
        return clone_thread(frame, flags, new_sp, parent_tid, child_tid, tls);
    Process* parent = current();
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
    memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
    child->cwd = parent->cwd;
    child->umask = parent->umask;
    for (int i = 0; i <= MAX_SIGNALS; i++)
        child->sigactions[i] = parent->sigactions[i];
    child->exit_signal = static_cast<int>(flags & 0xFF);
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
        child->pgid = parent->pgid;
        child->sid = parent->sid;
        child->parent = (flags & CLONE_PARENT) && parent->parent ? parent->parent : parent;
        child->parent->children.push_back(child);
        g_processes.push_back(child);
    }
    int pid = child->pid;

    // The child resumes from the same syscall, seeing 0 as the result.
    interrupts::Frame cf = *frame;
    cf.rax = 0;
    if (new_sp)
        cf.rsp = new_sp;
    child->start_ns = clock::now_ns();
    sched::Thread* t = make_user_thread(child, cf, pid);
    t->sigmask = parent_thread->sigmask;
    fpu::save(parent_thread->fpu_state);
    memcpy(t->fpu_state, parent_thread->fpu_state, fpu::state_size());
    t->fs_base = (flags & CLONE_SETTLS) ? tls : parent_thread->fs_base;
    t->gs_base = arch::rdmsr(MSR_KERNEL_GS_BASE);

    if (flags & CLONE_PARENT_SETTID)
        uaccess::put(reinterpret_cast<i32*>(parent_tid), pid);
    if (flags & CLONE_CHILD_SETTID)
        child->mem->write_remote(child_tid, &pid, sizeof(i32));
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = child_tid;

    sched::start(t);

    if (flags & CLONE_VFORK)
        child->vfork_done.wait(); // the child can't be reaped before we return: we're its parent
    return pid;
}

// ---- execve ----------------------------------------------------------------

namespace {

long exec_impl(interrupts::Frame* frame, const char* path, const char* const* argv, const char* const* envp,
               int depth) {
    Process* p = current();
    vfs::Inode* inode;
    if (long r = vfs::resolve(path, p->cwd, true, &inode); r < 0)
        return r;
    if (!vfs::is_reg(inode->mode))
        return -err::ACCES;
    if (!(inode->mode & 0111))
        return -err::ACCES;

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

    auto* as = vmm::AddressSpace::create_user();
    if (!as)
        return -err::NOMEM;
    auto* mem = new Memory(as);
    elf::LoadResult loaded;
    if (long r = elf::load(mem, inode, path, argv, envp, &loaded); r < 0) {
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
    Memory* old = p->mem;
    {
        InterruptGuard guard;
        p->mem = mem;
        t->aspace = as;
        as->activate();
    }
    old->put(); // a vfork parent may still hold it

    snprintf(p->name, sizeof(p->name), "%s", basename(path));
    snprintf(p->exe_path, sizeof(p->exe_path), "%s", path);
    snprintf(t->name, sizeof(t->name), "%s", p->name);
    t->fs_base = 0;
    t->gs_base = 0;
    arch::wrmsr(MSR_FS_BASE, 0);
    arch::wrmsr(MSR_KERNEL_GS_BASE, 0);
    fpu::reset(t->fpu_state);
    fpu::restore(t->fpu_state);

    *frame = initial_frame(loaded.entry, loaded.stack_ptr);
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

// ---- wait4 -----------------------------------------------------------------

long do_wait4(int pid, int* status_out, int options) {
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
            for (Process* c : me->children) {
                bool match = pid > 0 ? c->pid == pid
                           : pid == -1 ? true
                           : pid == 0 ? c->pgid == me->pgid
                                      : c->pgid == -pid;
                if (!match)
                    continue;
                any = true;
                if (c->state == State::Zombie) {
                    reaped = c;
                    me->children.remove(c);
                    g_processes.remove(c);
                    break;
                }
            }
        }

        if (reaped) {
            int child_pid = reaped->pid;
            *status_out = reaped->exit_status;
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
    p->mem->put();
    p->mem = nullptr;
    me->process = nullptr; // from here on we're an ordinary kernel thread

    if (p->vfork_child)
        p->vfork_done.complete_all();

    Process* orphan_zombies[32];
    int orphan_count = 0;
    bool free_self = false;
    {
        LockGuard guard(g_table_lock);
        // Our children lose their parent. Zombies among them are freed now;
        // living ones free themselves when they exit.
        while (Process* c = p->children.pop_front()) {
            c->parent = nullptr;
            if (c->state == State::Zombie && orphan_count < 32) {
                g_processes.remove(c);
                orphan_zombies[orphan_count++] = c;
            }
        }
        p->exit_status = wait_status;
        p->state = State::Zombie;

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

sched::Thread* find_thread(int tid) {
    for (Process* p : g_processes) {
        LockGuard guard(p->thread_lock);
        for (sched::Thread* t : p->threads)
            if (t->user_tid == tid)
                return t;
    }
    return nullptr;
}

void kill_current_on_fault(const char* what, u64 address, u64 ip, int sig) {
    Process* p = current();
    klog::error("%s[%d]: %s at %#lx, ip %#lx - killed (signal %d)", p->name, p->pid, what, address, ip, sig);
    exit_current(sig);
}

} // namespace proc
