#include <core/log.hpp>
#include <drivers/serial.hpp>
#include <lib/format.hpp>
#include <lib/spinlock.hpp>
#include <time/clock.hpp>
#include <video/console.hpp>

namespace {

IrqSpinlock g_print_lock;

void output_sink(char c, void*) {
    serial::putc(c);
    console::putc(c);
}

int vprint_unlocked(const char* fmt, va_list args) { return vformat(output_sink, nullptr, fmt, args); }

int print_unlocked(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vprint_unlocked(fmt, args);
    va_end(args);
    return n;
}

// Ring buffer of log lines (oldest bytes are overwritten).
constexpr usize RING = 128 * 1024;
char g_ring[RING];
usize g_ring_start = 0, g_ring_len = 0;

void ring_sink(char c, void*) {
    if (g_ring_len < RING) {
        g_ring[(g_ring_start + g_ring_len++) % RING] = c;
    } else {
        g_ring[g_ring_start] = c;
        g_ring_start = (g_ring_start + 1) % RING;
    }
}

void ring_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vformat(ring_sink, nullptr, fmt, args);
    va_end(args);
}

void log_line(int level, const char* tag, const char* fmt, va_list args) {
    LockGuard guard(g_print_lock);
    va_list copy;
    va_copy(copy, args);
    print_unlocked("%s ", tag);
    vprint_unlocked(fmt, args);
    print_unlocked("\n");
    u64 us = clock::now_ns() / 1000;
    ring_printf("<%d>[%5lu.%06lu] ", level, us / 1'000'000, us % 1'000'000);
    vformat(ring_sink, nullptr, fmt, copy);
    ring_sink('\n', nullptr);
    va_end(copy);
}

} // namespace

int kvprintf(const char* fmt, va_list args) {
    LockGuard guard(g_print_lock);
    return vprint_unlocked(fmt, args);
}

int kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = kvprintf(fmt, args);
    va_end(args);
    return n;
}

namespace {
void serial_sink(char c, void*) { serial::putc(c); }
} // namespace

void kwrite(const char* s, usize len) {
    LockGuard guard(g_print_lock);
    serial::write(s, len);
    console::write(s, len);
}

int kprintf_serial(const char* fmt, ...) {
    LockGuard guard(g_print_lock);
    va_list args;
    va_start(args, fmt);
    int n = vformat(serial_sink, nullptr, fmt, args);
    va_end(args);
    return n;
}

namespace klog {

#define DEFINE_LOG_FN(name, level, tag)  \
    void name(const char* fmt, ...) {    \
        va_list args;                    \
        va_start(args, fmt);             \
        log_line(level, tag, fmt, args); \
        va_end(args);                    \
    }

// Levels as in Linux: 7 debug, 6 info, 4 warning, 3 error.
DEFINE_LOG_FN(debug, 7, "\x1b[90m[ DEBUG ]\x1b[0m")
DEFINE_LOG_FN(info,  6, "\x1b[94m[ INFO  ]\x1b[0m")
DEFINE_LOG_FN(ok,    6, "\x1b[92m[  OK   ]\x1b[0m")
DEFINE_LOG_FN(warn,  4, "\x1b[93m[ WARN  ]\x1b[0m")
DEFINE_LOG_FN(error, 3, "\x1b[91m[ ERROR ]\x1b[0m")

#undef DEFINE_LOG_FN

usize read_buffer(char* out, usize max) {
    LockGuard guard(g_print_lock);
    usize n = g_ring_len < max ? g_ring_len : max;
    usize skip = g_ring_len - n; // keep the newest bytes
    for (usize i = 0; i < n; i++)
        out[i] = g_ring[(g_ring_start + skip + i) % RING];
    return n;
}

usize buffer_length() {
    LockGuard guard(g_print_lock);
    return g_ring_len;
}

void clear_buffer() {
    LockGuard guard(g_print_lock);
    g_ring_start = g_ring_len = 0;
}

void break_lock() { g_print_lock.force_unlock(); }

} // namespace klog
