#pragma once

// Lockup detector: stuck CPUs print where they are (NMI).
namespace watchdog {
void start();
void request_thread_dump(); // Alt+SysRq+W: every thread's call chain into the log, then saved to the stick
}
