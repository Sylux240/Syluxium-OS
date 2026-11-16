// Time and sleeping.

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
    case CLOCK_PROCESS_CPUTIME_ID: // TODO: real per-process CPU accounting
    case CLOCK_THREAD_CPUTIME_ID:
        *out = clock::now_ns();
        return 0;
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

void register_time() {
    set(35, sys_nanosleep, "nanosleep");
    set(96, sys_gettimeofday, "gettimeofday");
    set(201, sys_time, "time");
    set(228, sys_clock_gettime, "clock_gettime");
    set(229, sys_clock_getres, "clock_getres");
    set(230, sys_clock_nanosleep, "clock_nanosleep");
}

} // namespace syscall
