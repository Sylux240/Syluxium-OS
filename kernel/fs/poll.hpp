#pragma once

#include <lib/types.hpp>

// Readiness notification for poll/select.
//
// Simple and correct rather than scalable: every source of readiness
// (terminal input, pipe data, ...) bumps one global sequence number and
// wakes everyone polling; pollers then re-check their own files. A per-file
// wait-queue scheme (like Linux's poll tables) can replace it later.
namespace poll {

constexpr u16 POLLIN = 0x001, POLLPRI = 0x002, POLLOUT = 0x004, POLLERR = 0x008, POLLHUP = 0x010,
              POLLNVAL = 0x020, POLLRDNORM = 0x040, POLLWRNORM = 0x100;

void notify();      // something became readable/writable somewhere

u64 sequence();     // current change counter
// Sleep until the counter moves past `seen`, a signal arrives, or the
// timeout passes (timeout_ns = ~0 waits forever). Returns false on timeout.
bool wait_for_change(u64 seen, u64 timeout_ns);

} // namespace poll
