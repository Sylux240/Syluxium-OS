#pragma once

#include <lib/types.hpp>

// Lockup detector: stuck CPUs print where they are (NMI).
namespace watchdog {
void start();
void request_thread_dump(); // Alt+SysRq+W: every thread's call chain into the log, then saved to the stick

// Operations that must not hang (a driver's probe or unplug, a shutdown
// step): one still running after `limit_s` seconds is logged with every
// thread's call chain, once, and the log saved to the stick - no keys
// needed, which matters when the screen is frozen or blank.
class Guard {
public:
    Guard(const char* what, const char* detail, u32 limit_s);
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    int m_slot;
};
}
