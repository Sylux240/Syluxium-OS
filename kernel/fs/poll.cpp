#include <fs/poll.hpp>
#include <proc/signal.hpp>
#include <sched/sync.hpp>

namespace poll {

namespace {
sched::WaitQueue g_waiters;
u64 g_sequence = 0; // protected by g_waiters' lock
} // namespace

void notify() {
    {
        LockGuard guard(g_waiters.lock());
        g_sequence++;
    }
    g_waiters.wake_all();
}

u64 sequence() {
    LockGuard guard(g_waiters.lock());
    return g_sequence;
}

bool wait_for_change(u64 seen, u64 timeout_ns) {
    auto changed = [&] { return g_sequence != seen || signal::interrupted(); };
    if (timeout_ns == ~0ull) {
        g_waiters.wait_until(changed);
        return true;
    }
    return g_waiters.wait_until_timeout(changed, timeout_ns);
}

} // namespace poll
