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

// Who a process is: real, effective, saved and file-system user/group ids,
// plus supplementary groups (as on Linux; uid 0 = root, who may do anything).
constexpr u32 NGROUPS_MAX = 32;
struct Cred {
    u32 ruid = 0, euid = 0, suid = 0, fsuid = 0;
    u32 rgid = 0, egid = 0, sgid = 0, fsgid = 0;
    u32 ngroups = 0;
    u32 groups[NGROUPS_MAX] = {};
};

constexpr int MAY_EXEC = 1, MAY_WRITE = 2, MAY_READ = 4;
const Cred& cred();          // the current process's (root for kernel threads)
bool capable();              // effective uid 0: privileged operations
bool in_group(u32 gid);      // file-system gid or a supplementary group
// Unix permission check (owner / group / other bits) for the current process,
// by its file-system ids, or by its real ids (access()). 0 or -EACCES.
long permission(vfs::Inode* n, int mask, bool real_ids = false);
bool owns(vfs::Inode* n);    // file-system uid owns it, or root
// May the caller remove or rename `victim` out of `dir`? (write + search on
// dir, and the sticky bit: only the file's or the directory's owner).
long may_delete(vfs::Inode* dir, vfs::Inode* victim);
// A freshly created inode belongs to the caller (group: the directory's if
// it's setgid); persisted through Inode::set_owner.
void init_owner(vfs::Inode* dir, vfs::Inode* n);

// A PID namespace (clone(CLONE_NEWPID), containers): its processes see
// each other by their own numbers starting at 1, and its first process
// (pid 1) adopts the orphans. One level: namespaces are made from the
// system's, not from inside another.
struct PidNs {
    Process* init = nullptr; // pid 1 (nullptr once it's gone: the namespace is dying)
    int next = 1;            // next pid number (table lock)
    u32 refs = 1;            // one per process in it
};

// Host and domain name (UTS namespace): shared until unshare(CLONE_NEWUTS).
struct Uts {
    char nodename[65] = "syluxium";
    char domainname[65] = "(none)";
    u32 refs = 1;
};
Uts* system_uts();
// setns(): the process and namespace kind a /proc/<pid>/ns/<kind> file names (procfs).
long ns_file_target(vfs::File* f, int* pid, char* kind, usize size);
void uts_get(Uts* u);
void uts_put(Uts* u);

struct Process {
    int pid = 0;
    int pgid = 0;   // process group (job)
    int sid = 0;    // session
    vfs::Inode* ctty = nullptr; // controlling terminal (what /dev/tty opens); nullptr = the console
    char name[64] = {};
    char exe_path[256] = {};

    Memory* mem = nullptr;
    bool compat = false;  // a 32-bit (ia32) program: compat syscalls, 32-bit signal frames, < 4 GiB

    vfs::File* fds[MAX_FDS] = {};
    bool cloexec[MAX_FDS] = {};
    sched::Mutex fd_lock;

    vfs::Inode* cwd = nullptr;
    char cwd_path[512] = "/";
    vfs::Inode* root = nullptr; // chroot / pivot_root: this process's "/" (nullptr: the system's)
    PidNs* pidns = nullptr;     // nullptr: the system's PID namespace
    void* mntns = nullptr;      // mount namespace (vfs table); nullptr: the system's
    int vpid = 0;               // pid inside pidns (= pid in the system's)
    PidNs* child_pidns = nullptr; // setns(pid): where our children will live
    Uts* uts = nullptr;         // nullptr: the system's
    // ITIMER_REAL / alarm(): absolute deadline (clock ns, 0 = off) and reload interval.
    u64 alarm_deadline = 0, alarm_interval = 0;
    u32 umask = 022;
    u32 personality = 0; // personality(2); ADDR_NO_RANDOMIZE turns ASLR off for its execs
    Cred cred;
    int oom_score_adj = 0; // /proc/<pid>/oom_score_adj (kept, not used: no OOM killer yet)
    // LinuxKPI's view of this process (binder): its task_struct (the threads'
    // group_leader) and mm_struct, created on first use, released with it.
    void* lkpi_task = nullptr;
    void* lkpi_mm = nullptr;
    ~Process();

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
    i32 sig_code[65] = {}, sig_pid[65] = {}; u32 sig_uid[65] = {}; // who sent each (siginfo)
    u64 sig_value[65] = {}; i32 sig_timer[65] = {}; // POSIX timer signals: sigev_value and timer id
    SigAction sigactions[MAX_SIGNALS + 1];  // shared by all threads (per-thread masks live in Thread)
};

Process* current();
Process* find(int pid); // caller must hold table_lock()
// PID namespaces. All take the table lock themselves unless noted.
int pid_nr(const Process* p, const Process* viewer);  // p's pid as viewer sees it, 0 if invisible
Process* find_visible(int pid, const Process* viewer); // by a pid viewer uses; caller holds table_lock()
bool visible(const Process* p, const Process* viewer); // same namespace, or viewer in the system's
int pgid_nr(int global_pgid, const Process* viewer);   // a process group / session id, translated
int pgid_global(int pgid, const Process* viewer);      // and back (0 if no such group is visible)
int tid_nr(const sched::Thread* t, const Process* viewer);
sched::Thread* find_visible_thread(int tid, const Process* viewer); // caller holds table_lock()
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
long do_wait4(int pid, int* status_out, int options, u32* uid_out = nullptr);

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
// Swap the file in a used slot (the old one is released). For LinuxKPI fd_install.
long fd_replace(Process* p, int fd, vfs::File* f);
long fd_dup2(Process* p, int old_fd, int new_fd, bool cloexec);

void init();
u32 count();

} // namespace proc
