// Time and sleeping.

#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/uaccess.hpp>
#include <proc/signal.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <syscall/syscall.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace syscall {

namespace {

struct Timespec {
    i64 sec;
    i64 nsec;
};

struct Timeval {
    i64 sec;
    i64 usec;
};

constexpr int CLOCK_REALTIME = 0, CLOCK_MONOTONIC = 1, CLOCK_PROCESS_CPUTIME_ID = 2,
              CLOCK_THREAD_CPUTIME_ID = 3, CLOCK_MONOTONIC_RAW = 4, CLOCK_REALTIME_COARSE = 5,
              CLOCK_MONOTONIC_COARSE = 6, CLOCK_BOOTTIME = 7, CLOCK_TAI = 11;
constexpr u32 TIMER_ABSTIME = 1;

// CPU time from the scheduler's tick accounting (1 kHz: 1 tick = 1 ms).
constexpr u64 NS_PER_TICK = 1'000'000;

void process_cpu_ticks(proc::Process* p, u64* user, u64* sys) {
    *user = *sys = 0;
    LockGuard guard(p->thread_lock);
    for (sched::Thread* t : p->threads) {
        *user += t->utime_ticks;
        *sys += t->stime_ticks;
    }
}

long clock_ns(int id, u64* out) {
    switch (id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_TAI:
        *out = rtc::realtime_ns();
        return 0;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        *out = clock::now_ns();
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID: { // CPU time actually used (all threads), not wall time
        u64 user, sys;
        process_cpu_ticks(proc::current(), &user, &sys);
        *out = (user + sys) * NS_PER_TICK;
        return 0;
    }
    case CLOCK_THREAD_CPUTIME_ID: {
        sched::Thread* t = sched::current();
        *out = (t->utime_ticks + t->stime_ticks) * NS_PER_TICK;
        return 0;
    }
    default:
        return -err::INVAL;
    }
}

Timespec to_timespec(u64 ns) { return {static_cast<i64>(ns / 1'000'000'000), static_cast<i64>(ns % 1'000'000'000)}; }

// Sleep until the clock reaches `target_ns`; a signal ends the sleep early with -EINTR.
long sleep_until(int id, u64 target_ns) {
    sched::WaitQueue wq;
    for (;;) {
        u64 now;
        clock_ns(id, &now);
        if (now >= target_ns)
            return 0;
        if (wq.wait_until_timeout([] { return signal::interrupted(); }, target_ns - now))
            return -err::INTR;
    }
}

long sys_clock_gettime(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 ns;
    if (long r = clock_ns(static_cast<int>(a0), &ns); r < 0)
        return r;
    return uaccess::put(reinterpret_cast<Timespec*>(a1), to_timespec(ns));
}

long sys_clock_getres(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 ns;
    if (long r = clock_ns(static_cast<int>(a0), &ns); r < 0)
        return r;
    if (!a1)
        return 0;
    return uaccess::put(reinterpret_cast<Timespec*>(a1), Timespec{0, 1});
}

long sys_gettimeofday(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!a0)
        return 0;
    u64 ns = rtc::realtime_ns();
    return uaccess::put(reinterpret_cast<Timeval*>(a0),
                        Timeval{static_cast<i64>(ns / 1'000'000'000), static_cast<i64>(ns % 1'000'000'000 / 1000)});
}

long sys_time(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    i64 t = static_cast<i64>(rtc::realtime_ns() / 1'000'000'000);
    if (a0) {
        if (long r = uaccess::put(reinterpret_cast<i64*>(a0), t); r < 0)
            return r;
    }
    return t;
}

long sys_nanosleep(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Timespec req;
    if (long r = uaccess::get(&req, reinterpret_cast<const Timespec*>(a0)); r < 0)
        return r;
    if (req.sec < 0 || req.nsec < 0 || req.nsec >= 1'000'000'000)
        return -err::INVAL;
    u64 ns = static_cast<u64>(req.sec) * 1'000'000'000 + static_cast<u64>(req.nsec);
    return sleep_until(CLOCK_MONOTONIC, clock::now_ns() + ns);
}

long sys_clock_nanosleep(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int id = static_cast<int>(a0);
    Timespec req;
    if (long r = uaccess::get(&req, reinterpret_cast<const Timespec*>(a2)); r < 0)
        return r;
    if (req.sec < 0 || req.nsec < 0 || req.nsec >= 1'000'000'000)
        return -err::INVAL;
    u64 ns = static_cast<u64>(req.sec) * 1'000'000'000 + static_cast<u64>(req.nsec);
    u64 now;
    if (long r = clock_ns(id, &now); r < 0)
        return r;
    return sleep_until(id, (a1 & TIMER_ABSTIME) ? ns : now + ns);
}

} // namespace

// ---- alarm / setitimer (ITIMER_REAL only) ----

namespace {

struct ITimerVal {
    i64 interval_sec, interval_usec, value_sec, value_usec;
};

bool g_alarm_thread_started = false;

void alarm_thread(void*) {
    for (;;) {
        sched::sleep_ms(2);
        u64 now = clock::now_ns();
        proc::for_each([](proc::Process* p, void* ctx) {
            u64 t = *static_cast<u64*>(ctx);
            if (p->alarm_deadline && t >= p->alarm_deadline) {
                p->alarm_deadline = p->alarm_interval ? t + p->alarm_interval : 0;
                signal::send(p, signal::SIGALRM);
            }
        }, &now);
    }
}

void ensure_alarm_thread() {
    if (!__atomic_exchange_n(&g_alarm_thread_started, true, __ATOMIC_ACQ_REL))
        sched::spawn("itimer", alarm_thread, nullptr, -1, true);
}

ITimerVal current_itimer(proc::Process* p) {
    ITimerVal v{};
    u64 now = clock::now_ns();
    if (p->alarm_deadline > now) {
        u64 left = p->alarm_deadline - now;
        v.value_sec = static_cast<i64>(left / 1'000'000'000);
        v.value_usec = static_cast<i64>(left % 1'000'000'000 / 1000);
        if (!v.value_sec && !v.value_usec)
            v.value_usec = 1;
    }
    v.interval_sec = static_cast<i64>(p->alarm_interval / 1'000'000'000);
    v.interval_usec = static_cast<i64>(p->alarm_interval % 1'000'000'000 / 1000);
    return v;
}

long sys_setitimer(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 != 0)
        return -err::INVAL; // ITIMER_VIRTUAL / ITIMER_PROF: not supported
    proc::Process* p = proc::current();
    ITimerVal nv{};
    if (a1)
        if (long r = uaccess::get(&nv, reinterpret_cast<ITimerVal*>(a1)); r < 0)
            return r;
    if (a2)
        if (long r = uaccess::put(reinterpret_cast<ITimerVal*>(a2), current_itimer(p)); r < 0)
            return r;
    u64 value = static_cast<u64>(nv.value_sec) * 1'000'000'000ull + static_cast<u64>(nv.value_usec) * 1000;
    p->alarm_interval = static_cast<u64>(nv.interval_sec) * 1'000'000'000ull + static_cast<u64>(nv.interval_usec) * 1000;
    p->alarm_deadline = value ? clock::now_ns() + value : 0;
    if (value)
        ensure_alarm_thread();
    return 0;
}

long sys_getitimer(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 != 0)
        return -err::INVAL;
    return uaccess::put(reinterpret_cast<ITimerVal*>(a1), current_itimer(proc::current()));
}

long sys_alarm(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Process* p = proc::current();
    ITimerVal old = current_itimer(p);
    long left = old.value_sec + (old.value_usec ? 1 : 0);
    p->alarm_interval = 0;
    p->alarm_deadline = a0 ? clock::now_ns() + a0 * 1'000'000'000ull : 0;
    if (a0)
        ensure_alarm_thread();
    return left;
}

} // namespace

// ---- POSIX timers (timer_create & co.): a signal to the process (or one
// thread) when they expire. SIGEV_THREAD is done by the C library on top of
// SIGEV_THREAD_ID; the timer's sigev_value travels with the signal. ----

namespace {

struct PosixTimer {
    int id;
    int pid;           // owner (system pid) and its start time: the pid may be reused
    u64 start_ns;
    int clock;         // CLOCK_REALTIME (0) or a monotonic one
    int signo;         // 0 = SIGEV_NONE
    int tid;           // SIGEV_THREAD_ID: the thread (system tid); 0 = the process
    u64 value;         // sigev_value
    u64 deadline = 0;  // monotonic ns; 0 = disarmed
    u64 interval = 0;
    u32 overrun = 0;
    PosixTimer* next = nullptr;
};

IrqSpinlock g_ptimers_lock;
PosixTimer* g_ptimers = nullptr;
int g_next_timer_id = 1;
bool g_ptimer_thread_started = false;

void ptimer_thread(void*) {
    for (;;) {
        sched::sleep_ms(1);
        u64 now = clock::now_ns();
        LockGuard guard(g_ptimers_lock);
        for (PosixTimer* t = g_ptimers; t; t = t->next) {
            if (!t->deadline || now < t->deadline)
                continue;
            if (t->interval) {
                u64 late = (now - t->deadline) / t->interval;
                t->overrun = static_cast<u32>(late);
                t->deadline += (late + 1) * t->interval;
            } else {
                t->deadline = 0;
            }
            if (!t->signo)
                continue;
            LockGuard pguard(proc::table_lock());
            proc::Process* p = proc::find(t->pid);
            if (!p || p->start_ns != t->start_ns || p->state == proc::State::Zombie)
                continue;
            p->sig_value[t->signo] = t->value;
            p->sig_timer[t->signo] = t->id;
            if (t->tid) {
                if (sched::Thread* th = proc::find_thread(t->tid); th && th->process == p) {
                    signal::send_thread(th, t->signo, signal::SI_TIMER);
                    continue;
                }
            }
            signal::send(p, t->signo, signal::SI_TIMER);
        }
    }
}

// Under g_ptimers_lock: the caller's timer `id`.
PosixTimer* find_ptimer(int id) {
    proc::Process* me = proc::current();
    for (PosixTimer* t = g_ptimers; t; t = t->next)
        if (t->id == id && t->pid == me->pid && t->start_ns == me->start_ns)
            return t;
    return nullptr;
}

struct ITimerSpec64 {
    i64 interval_sec, interval_nsec, value_sec, value_nsec;
};

u64 clock_now(int clock) { return clock == 0 ? rtc::realtime_ns() : clock::now_ns(); }

long sys_timer_create(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int clock = static_cast<int>(a0);
    if (clock != 0 && clock != 1 && clock != 4 && clock != 7) // REALTIME, MONOTONIC, MONOTONIC_RAW, BOOTTIME
        return -err::INVAL;
    constexpr int SIGEV_SIGNAL = 0, SIGEV_NONE = 1, SIGEV_THREAD_ID = 4;
    struct SigEvent {
        u64 value;
        i32 signo, notify;
        i32 tid;
        u8 pad[44];
    } ev{};
    ev.signo = 14; // SIGALRM by default
    ev.notify = SIGEV_SIGNAL;
    if (a1 && uaccess::copy_from(&ev, reinterpret_cast<const void*>(a1), sizeof(ev)) < 0)
        return -err::FAULT;
    if (ev.notify != SIGEV_SIGNAL && ev.notify != SIGEV_NONE && ev.notify != SIGEV_THREAD_ID)
        return -err::INVAL;
    if (ev.notify != SIGEV_NONE && (ev.signo < 1 || ev.signo > proc::MAX_SIGNALS))
        return -err::INVAL;
    proc::Process* me = proc::current();
    auto* t = new PosixTimer;
    t->pid = me->pid;
    t->start_ns = me->start_ns;
    t->clock = clock;
    t->signo = ev.notify == SIGEV_NONE ? 0 : ev.signo;
    t->value = a1 ? ev.value : 0;
    if (ev.notify == SIGEV_THREAD_ID) {
        LockGuard pguard(proc::table_lock());
        sched::Thread* th = proc::find_visible_thread(ev.tid, me);
        if (!th || th->process != me) {
            delete t;
            return -err::INVAL;
        }
        t->tid = static_cast<int>(th->user_tid);
    }
    {
        LockGuard guard(g_ptimers_lock);
        t->id = g_next_timer_id++;
        if (!a1)
            t->value = static_cast<u64>(t->id);
        t->next = g_ptimers;
        g_ptimers = t;
    }
    if (!__atomic_exchange_n(&g_ptimer_thread_started, true, __ATOMIC_ACQ_REL))
        sched::spawn("posix-timers", ptimer_thread, nullptr, -1, true);
    i32 id = t->id;
    if (long r = uaccess::put(reinterpret_cast<i32*>(a2), id); r < 0)
        return r;
    return 0;
}

ITimerSpec64 ptimer_current(PosixTimer* t) {
    ITimerSpec64 s{};
    s.interval_sec = static_cast<i64>(t->interval / 1'000'000'000);
    s.interval_nsec = static_cast<i64>(t->interval % 1'000'000'000);
    if (t->deadline) {
        u64 now = clock::now_ns();
        u64 left = t->deadline > now ? t->deadline - now : 1;
        s.value_sec = static_cast<i64>(left / 1'000'000'000);
        s.value_nsec = static_cast<i64>(left % 1'000'000'000);
    }
    return s;
}

long sys_timer_settime(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u64 TIMER_ABSTIME = 1;
    ITimerSpec64 nv;
    if (long r = uaccess::copy_from(&nv, reinterpret_cast<const void*>(a2), sizeof(nv)); r < 0)
        return r;
    if (nv.value_nsec < 0 || nv.value_nsec >= 1'000'000'000 || nv.interval_nsec < 0 ||
        nv.interval_nsec >= 1'000'000'000 || nv.value_sec < 0 || nv.interval_sec < 0)
        return -err::INVAL;
    ITimerSpec64 old;
    {
        LockGuard guard(g_ptimers_lock);
        PosixTimer* t = find_ptimer(static_cast<int>(a0));
        if (!t)
            return -err::INVAL;
        old = ptimer_current(t);
        u64 value = static_cast<u64>(nv.value_sec) * 1'000'000'000 + static_cast<u64>(nv.value_nsec);
        t->interval = static_cast<u64>(nv.interval_sec) * 1'000'000'000 + static_cast<u64>(nv.interval_nsec);
        t->overrun = 0;
        if (!value) {
            t->deadline = 0;
        } else if (a1 & TIMER_ABSTIME) {
            u64 now_c = clock_now(t->clock), now_m = clock::now_ns();
            t->deadline = value > now_c ? now_m + (value - now_c) : now_m;
            if (!t->deadline)
                t->deadline = 1;
        } else {
            t->deadline = clock::now_ns() + value;
        }
    }
    if (a3)
        return uaccess::copy_to(reinterpret_cast<void*>(a3), &old, sizeof(old));
    return 0;
}

long sys_timer_gettime(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    ITimerSpec64 cur;
    {
        LockGuard guard(g_ptimers_lock);
        PosixTimer* t = find_ptimer(static_cast<int>(a0));
        if (!t)
            return -err::INVAL;
        cur = ptimer_current(t);
    }
    return uaccess::copy_to(reinterpret_cast<void*>(a1), &cur, sizeof(cur));
}

long sys_timer_getoverrun(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    LockGuard guard(g_ptimers_lock);
    PosixTimer* t = find_ptimer(static_cast<int>(a0));
    return t ? static_cast<long>(t->overrun) : -err::INVAL;
}

long sys_timer_delete(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    PosixTimer* gone = nullptr;
    {
        LockGuard guard(g_ptimers_lock);
        PosixTimer* t = find_ptimer(static_cast<int>(a0));
        if (!t)
            return -err::INVAL;
        for (PosixTimer** link = &g_ptimers; *link; link = &(*link)->next) {
            if (*link == t) {
                *link = t->next;
                gone = t;
                break;
            }
        }
    }
    delete gone;
    return 0;
}

} // namespace

// getrusage(who, usage): CPU times and peak memory.
long sys_getrusage(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    struct Rusage {
        i64 utime_sec, utime_usec, stime_sec, stime_usec;
        i64 maxrss, ixrss, idrss, isrss, minflt, majflt, nswap, inblock, oublock, msgsnd, msgrcv, nsignals, nvcsw,
            nivcsw;
    } ru{};
    static_assert(sizeof(Rusage) == 144);
    int who = static_cast<int>(a0);
    u64 user = 0, sys = 0;
    if (who == 0) { // RUSAGE_SELF
        process_cpu_ticks(proc::current(), &user, &sys);
    } else if (who == 1) { // RUSAGE_THREAD
        user = sched::current()->utime_ticks;
        sys = sched::current()->stime_ticks;
    } else if (who != -1) { // RUSAGE_CHILDREN: not accumulated yet (0)
        return -err::INVAL;
    }
    ru.utime_sec = static_cast<i64>(user / 1000);
    ru.utime_usec = static_cast<i64>(user % 1000 * 1000);
    ru.stime_sec = static_cast<i64>(sys / 1000);
    ru.stime_usec = static_cast<i64>(sys % 1000 * 1000);
    if (who != -1)
        ru.maxrss = static_cast<i64>(proc::current()->mem->rss_pages() * 4); // KiB (current, not peak)
    return uaccess::put(reinterpret_cast<Rusage*>(a1), ru);
}

// times(buf): CPU times in clock ticks (USER_HZ = 100); returns ticks since boot.
long sys_times(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0) {
        u64 user, sys;
        process_cpu_ticks(proc::current(), &user, &sys);
        i64 tms[4] = {static_cast<i64>(user / 10), static_cast<i64>(sys / 10), 0, 0};
        if (long r = uaccess::copy_to(reinterpret_cast<void*>(a0), tms, sizeof(tms)); r < 0)
            return r;
    }
    return static_cast<long>(clock::now_ns() / 10'000'000);
}

void register_time() {
    set(98, sys_getrusage, "getrusage");
    set(100, sys_times, "times");
    set(36, sys_getitimer, "getitimer");
    set(37, sys_alarm, "alarm");
    set(38, sys_setitimer, "setitimer");
    set(35, sys_nanosleep, "nanosleep");
    set(96, sys_gettimeofday, "gettimeofday");
    set(201, sys_time, "time");
    set(228, sys_clock_gettime, "clock_gettime");
    set(229, sys_clock_getres, "clock_getres");
    set(230, sys_clock_nanosleep, "clock_nanosleep");
    set(222, sys_timer_create, "timer_create");
    set(223, sys_timer_settime, "timer_settime");
    set(224, sys_timer_gettime, "timer_gettime");
    set(225, sys_timer_getoverrun, "timer_getoverrun");
    set(226, sys_timer_delete, "timer_delete");
}

} // namespace syscall
