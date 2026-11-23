#include <arch/x86_64/cpu.hpp>
#include <boot/limine_requests.hpp>
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
constexpr usize RING = 512 * 1024;
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

// Identical lines in a row are collapsed, as Linux's logging does ("last
// message repeated N times"): a driver's debug output can repeat one line
// thousands of times (amdgpu's "Allow idle optimizations (MALL)" toggles
// many times a second) and bury everything else in the ring and on the stick.
char g_last[1024], g_prev[1024]; // the last two lines (debug lines can alternate in a pair: "true"/"false")
char g_text[1024]; // the line being logged (under g_print_lock)
int g_last_level = -1;
u32 g_repeats = 0;
bool g_pair = false; // the repeats alternate between the last two lines

int serial_level();
void console_sink(char c, void*);
// print_unlocked for a line of `level`: serial and screen, or screen only.
void print_level(int level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (level <= serial_level())
        vprint_unlocked(fmt, args);
    else
        vformat(console_sink, nullptr, fmt, args);
    va_end(args);
}

void flush_repeats() {
    if (!g_repeats)
        return;
    u64 us = clock::now_ns() / 1000;
    const char* lines = g_pair ? "lines" : "line";
    const char* times = g_repeats == 1 ? "once more" : "times";
    if (g_repeats == 1) {
        print_level(g_last_level, "  (the last %s repeated once more)\n", lines);
        ring_printf("<%d>[%5lu.%06lu] (the last %s repeated once more)\n", g_last_level, us / 1'000'000,
                    us % 1'000'000, lines);
    } else {
        print_level(g_last_level, "  (the last %s repeated %u %s)\n", lines, g_repeats, times);
        ring_printf("<%d>[%5lu.%06lu] (the last %s repeated %u %s)\n", g_last_level, us / 1'000'000,
                    us % 1'000'000, lines, g_repeats, times);
    }
    g_pair = false;
    g_repeats = 0;
}

// Which log lines go to the serial port. A real UART at 115200 baud takes
// ~9 ms per line, all of it with interrupts off under the log lock: on a PC
// with a serial port (an H97 board) every debug line (DRM's mode-setting
// debug, drmtrace) stalled the CPUs that much, and an X server start that
// logs thousands of lines froze the machine. As Linux (whose console
// leaves out debug lines, and which uses no serial console unless asked):
// on real hardware only warnings and errors reach the serial port; boot
// option "serial" sends everything, as in a VM (where the port is instant).
// The log ring, dmesg and the stick's syluxium-log.txt keep every line.
int g_serial_level = -1;
int serial_level() {
    if (g_serial_level < 0) {
        bool vm = (arch::cpuid(1).ecx >> 31) & 1; // CPUID: running under a hypervisor
        const char* c = boot::cmdline();
        bool asked = false;
        for (const char* p = c; (p = __builtin_strstr(p, "serial")) != nullptr; p += 6)
            if ((p == c || p[-1] == ' ') && (p[6] == 0 || p[6] == ' '))
                asked = true;
        g_serial_level = vm || asked ? 7 : 4;
    }
    return g_serial_level;
}

void console_sink(char c, void*) { console::putc(c); }

void log_line(int level, const char* tag, const char* fmt, va_list args) {
    LockGuard guard(g_print_lock);
    char* text = g_text;
    vsnprintf(text, sizeof(g_text), fmt, args);
    if (level == g_last_level && (__builtin_strcmp(text, g_last) == 0 || (level == 7 && __builtin_strcmp(text, g_prev) == 0))) {
        if (__builtin_strcmp(text, g_last) != 0) { // the other line of an alternating pair
            g_pair = true;
            __builtin_memcpy(g_prev, g_last, sizeof(g_prev));
            __builtin_memcpy(g_last, text, sizeof(g_last));
        }
        g_repeats++;
        return;
    }
    flush_repeats();
    __builtin_memcpy(g_prev, g_last, sizeof(g_prev));
    __builtin_memcpy(g_last, text, sizeof(g_last));
    g_last_level = level;
    if (level <= serial_level()) {
        print_unlocked("%s %s\n", tag, text);
    } else {
        // Screen only (where the console still shows: before a GPU driver takes it).
        console::write(tag, __builtin_strlen(tag));
        console::putc(' ');
        console::write(text, __builtin_strlen(text));
        console::putc('\n');
    }
    u64 us = clock::now_ns() / 1000;
    ring_printf("<%d>[%5lu.%06lu] %s\n", level, us / 1'000'000, us % 1'000'000, text);
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
