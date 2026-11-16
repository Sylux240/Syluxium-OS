#include <core/log.hpp>
#include <drivers/serial.hpp>
#include <lib/format.hpp>
#include <lib/spinlock.hpp>
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

void log_line(const char* tag, const char* fmt, va_list args) {
    LockGuard guard(g_print_lock);
    print_unlocked("%s ", tag);
    vprint_unlocked(fmt, args);
    print_unlocked("\n");
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

#define DEFINE_LOG_FN(name, tag)       \
    void name(const char* fmt, ...) {  \
        va_list args;                  \
        va_start(args, fmt);           \
        log_line(tag, fmt, args);      \
        va_end(args);                  \
    }

DEFINE_LOG_FN(debug, "\x1b[90m[ DEBUG ]\x1b[0m")
DEFINE_LOG_FN(info,  "\x1b[94m[ INFO  ]\x1b[0m")
DEFINE_LOG_FN(ok,    "\x1b[92m[  OK   ]\x1b[0m")
DEFINE_LOG_FN(warn,  "\x1b[93m[ WARN  ]\x1b[0m")
DEFINE_LOG_FN(error, "\x1b[91m[ ERROR ]\x1b[0m")

#undef DEFINE_LOG_FN

void break_lock() { g_print_lock.force_unlock(); }

} // namespace klog
