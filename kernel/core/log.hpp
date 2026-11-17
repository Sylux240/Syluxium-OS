#pragma once

#include <lib/types.hpp>

// Kernel output: goes to the serial port and the framebuffer console.
int kprintf(const char* fmt, ...) PRINTF_LIKE(1, 2);
int kvprintf(const char* fmt, va_list args);
// Raw bytes to serial + screen under the log lock (terminal output).
void kwrite(const char* s, usize len);
// Serial port only (verbose details that would flood the screen).
int kprintf_serial(const char* fmt, ...) PRINTF_LIKE(1, 2);

namespace klog {

void debug(const char* fmt, ...) PRINTF_LIKE(1, 2);
void info(const char* fmt, ...) PRINTF_LIKE(1, 2);
void ok(const char* fmt, ...) PRINTF_LIKE(1, 2);
void warn(const char* fmt, ...) PRINTF_LIKE(1, 2);
void error(const char* fmt, ...) PRINTF_LIKE(1, 2);

// Copy of the kernel log ("<level>[seconds] text" lines, like Linux) for
// dmesg / syslog(2). Returns bytes copied; `size_only` returns the length.
usize read_buffer(char* out, usize max);
usize buffer_length();
void clear_buffer();

// Used by panic() so a fault while printing can't deadlock the panic message.
void break_lock();

} // namespace klog
