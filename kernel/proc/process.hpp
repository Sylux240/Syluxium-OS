#pragma once

#include <arch/x86_64/interrupts.hpp>
#include <fs/vfs.hpp>
#include <lib/list.hpp>
#include <proc/memory.hpp>
#include <sched/sync.hpp>

namespace proc {

constexpr int MAX_FDS = 1024;
constexpr int MAX_SIGNALS = 64;

struct SigAction {
    u64 handler = 0;  // 0 = SIG_DFL, 1 = SIG_IGN, else user function
    u64 flags = 0;
    u64 restorer = 0;
    u64 mask = 0;
};

enum class State : u8 { Running, Zombie };

struct Process {
    int pid = 0;
    int pgid = 0;   // process group (job)
    int sid = 0;    // session
    char name[64] = {};
    char exe_path[256] = {};

    Memory* mem = nullptr;
    bool compat = false;  // a 32-bit (ia32) program: compat syscalls, 32-bit signal frames, < 4 GiB

    vfs::File* fds[MAX_FDS] = {};
    bool cloexec[MAX_FDS] = {};
    sched::Mutex fd_lock;

    vfs::Inode* cwd = nullptr;
    char cwd_path[512] = "/";
    // ITIMER_REAL / alarm(): absolute deadline (clock ns, 0 = off) and reload interval.
    u64 alarm_deadline = 0, alarm_interval = 0;
    u32 umask = 022;

    sched::Thread* main_thread = nullptr;     // the thread the process started with

    // All threads of this process (the "thread group"); protected by thread_lock.
    IntrusiveList<sched::Thread, &sched::Thread::proc_node> threads;
    IrqSpinlock thread_lock;
    u32 live_threads = 0;
    bool group_exiting = false;               // exit_group() / fatal signal: all threads must exit
    int group_exit_status = 0;
    sched::Thread* zap_except = nullptr;      // execve: every thread but this one must exit
    sched::WaitQueue thread_exit_wait;        // woken whenever a thread of this process exits

    // Accounting.
    u64 start_ns = 0;                         // clock::now_ns() when created
    u64 dead_utime = 0, dead_stime = 0;       // ticks of threads that already exited

    // ---- Process tree (protected by the global process table lock) ----
    Process* parent = nullptr;           // null: started by the kernel or orphaned
    ListNode sibling_node;               // in parent->children
    ListNode table_node;                 // in the global table
    IntrusiveList<Process, &Process::sibling_node> children;
    State state = State::Running;
    int exit_status = 0;                 // wait() status word
    int exit_signal = 17;                // signal sent to the parent on exit (SIGCHLD)
    bool kernel_waited = false;          // a kernel thread will collect it with proc::wait()

    // Woken whenever a child changes state; `child_events` counts those changes.
    sched::WaitQueue child_wait;
    u64 child_events = 0;
    sched::Completion exit_done;         // for kernel waiters
    sched::Completion vfork_done;        // vfork parent sleeps until the child execs or exits
    bool vfork_child = false;

    // ---- Signals ----
    u64 sig_pending = 0;                 // process-wide pending signals, bit n-1 = signal n
    SigAction sigactions[MAX_SIGNALS + 1];  // shared by all threads (per-thread masks live in Thread)
};

Process* current();
Process* find(int pid); // caller must hold table_lock()
IrqSpinlock& table_lock();
// Call fn for every live process, with the table lock held (fn must not sleep).
void for_each(void (*fn)(Process* p, void* ctx), void* ctx);

// Load an ELF executable into a new process and start it (kernel API).
// argv[0] need not equal `path`.
long spawn(const char* path, const char* const* argv, const char* const* envp, Process** out);

// Wait for a process started with spawn() to exit; frees it; returns its wait status.
int wait(Process* p);

// System call implementations.
long do_clone(interrupts::Frame* frame, u64 flags, u64 new_sp, u64 parent_tid, u64 child_tid, u64 tls);
long do_execve(interrupts::Frame* frame, const char* path, const char* const* argv, const char* const* envp);
long do_wait4(int pid, int* status_out, int options);

// End the whole current process (all threads) with a wait() status word
// (exit: code << 8, killed: signal number).
[[noreturn]] void exit_current(int wait_status);
[[noreturn]] void exit_group(int wait_status);

// End only the calling thread (exit(2)); the last thread ending ends the process.
[[noreturn]] void exit_thread(int code);

// Find a thread by its Linux thread id; caller holds table_lock().
sched::Thread* find_thread(int tid);

// Kill the current process because of a fatal CPU exception in user mode.
[[noreturn]] void kill_current_on_fault(const char* what, u64 address, u64 ip, int signal);

// File descriptor table.
long fd_install(Process* p, vfs::File* f, int min_fd = 0, bool cloexec = false);
vfs::File* fd_get(Process* p, int fd); // returns a new reference (file_put when done)
long fd_close(Process* p, int fd);
long fd_dup2(Process* p, int old_fd, int new_fd, bool cloexec);

void init();
u32 count();

} // namespace proc
