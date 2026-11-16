#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <drivers/serial.hpp>
#include <drivers/tty.hpp>
#include <fs/poll.hpp>
#include <lib/string.hpp>
#include <proc/signal.hpp>
#include <sched/sync.hpp>
#include <video/console.hpp>

namespace tty {

namespace {

// Linux termios (the kernel-side struct used by TCGETS/TCSETS).
struct KernelTermios {
    u32 c_iflag, c_oflag, c_cflag, c_lflag;
    u8 c_line;
    u8 c_cc[19];
};
static_assert(sizeof(KernelTermios) == 36);

struct WinSize {
    u16 ws_row, ws_col, ws_xpixel, ws_ypixel;
};

constexpr u32 ICRNL = 0000400;
constexpr u32 OPOST = 0000001, ONLCR = 0000004;
constexpr u32 B38400 = 0000017, CS8 = 0000060, CREAD = 0000200;
constexpr u32 ISIG = 0000001, ICANON = 0000002, ECHO = 0000010, ECHOE = 0000020, ECHOK = 0000040, IEXTEN = 0100000;
constexpr int VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4;

constexpr u64 TCGETS = 0x5401, TCSETS = 0x5402, TCSETSW = 0x5403, TCSETSF = 0x5404;
constexpr u64 TIOCGPGRP = 0x540F, TIOCSPGRP = 0x5410, TIOCGWINSZ = 0x5413, TIOCSWINSZ = 0x5414;
constexpr u64 FIONREAD = 0x541B;
// termios2 variants (glibc 2.42+ uses these for tcgetattr/tcsetattr).
constexpr u64 TCGETS2 = 0x802C542A, TCSETS2 = 0x402C542B, TCSETSW2 = 0x402C542C, TCSETSF2 = 0x402C542D;

struct KernelTermios2 {
    KernelTermios base;
    u32 c_ispeed, c_ospeed;
};
static_assert(sizeof(KernelTermios2) == 44);

constexpr usize BUF_SIZE = 4096;

KernelTermios g_termios = {
    ICRNL, OPOST | ONLCR, B38400 | CS8 | CREAD, ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN, 0,
    {3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0},
};
i32 g_foreground_pgrp = 1;

// Cooked input waiting to be read, plus the line being edited.
char g_ready[BUF_SIZE];
usize g_ready_head = 0, g_ready_count = 0;
char g_line[BUF_SIZE];
usize g_line_len = 0;
IrqSpinlock g_input_lock;
sched::WaitQueue g_readers;

void echo(const char* s, usize n) {
    if (g_termios.c_lflag & ECHO)
        kwrite(s, n);
}

void push_ready(const char* s, usize n) {
    for (usize i = 0; i < n && g_ready_count < BUF_SIZE; i++)
        g_ready[(g_ready_head + g_ready_count++) % BUF_SIZE] = s[i];
}

void serial_irq(interrupts::Frame*) {
    while (arch::inb(0x3F8 + 5) & 1)
        input_char(static_cast<char>(arch::inb(0x3F8)));
}

class Console : public vfs::Inode {
public:
    Console() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0620;
        rdev = (5 << 8) | 1; // Linux: major 5, minor 1 = /dev/console
    }

    bool seekable() const override { return false; }

    isize write(u64, const void* buf, usize count) override {
        kwrite(static_cast<const char*>(buf), count);
        return static_cast<isize>(count);
    }

    isize read(u64, void* buf, usize count) override {
        if (count == 0)
            return 0;
        bool canonical = g_termios.c_lflag & ICANON;
        g_readers.wait_until([&] {
            if (signal::interrupted())
                return true;
            if (!canonical)
                return g_ready_count > 0 || g_line_len > 0;
            return g_ready_count > 0;
        });

        LockGuard guard(g_input_lock);
        if (g_ready_count == 0 && (canonical || g_line_len == 0))
            return -err::INTR; // woken by a signal (e.g. Ctrl+C)
        if (!canonical && g_line_len) { // raw mode: everything typed is available
            push_ready(g_line, g_line_len);
            g_line_len = 0;
        }
        auto* out = static_cast<char*>(buf);
        usize n = 0;
        while (n < count && g_ready_count) {
            char c = g_ready[g_ready_head];
            g_ready_head = (g_ready_head + 1) % BUF_SIZE;
            g_ready_count--;
            if (canonical && c == 4) // ^D: end of input (a read returning 0)
                break;
            out[n++] = c;
            if (canonical && c == '\n')
                break;
        }
        return static_cast<isize>(n);
    }

    u16 poll(vfs::File*) override {
        LockGuard guard(g_input_lock);
        bool readable = g_ready_count > 0 || (!(g_termios.c_lflag & ICANON) && g_line_len > 0);
        return poll::POLLOUT | (readable ? poll::POLLIN : 0);
    }

    long ioctl(u64 request, u64 arg) override {
        // `arg` is a user pointer, already validated by the syscall layer.
        switch (request) {
        case TCGETS:
            memcpy(reinterpret_cast<void*>(arg), &g_termios, sizeof(g_termios));
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            memcpy(&g_termios, reinterpret_cast<void*>(arg), sizeof(g_termios));
            return 0;
        case TCGETS2: {
            KernelTermios2 t2{g_termios, 38400, 38400};
            memcpy(reinterpret_cast<void*>(arg), &t2, sizeof(t2));
            return 0;
        }
        case TCSETS2:
        case TCSETSW2:
        case TCSETSF2:
            memcpy(&g_termios, reinterpret_cast<void*>(arg), sizeof(g_termios));
            return 0;
        case TIOCGWINSZ: {
            WinSize ws{static_cast<u16>(console::rows()), static_cast<u16>(console::cols()), 0, 0};
            if (ws.ws_row == 0) { ws.ws_row = 25; ws.ws_col = 80; }
            memcpy(reinterpret_cast<void*>(arg), &ws, sizeof(ws));
            return 0;
        }
        case TIOCSWINSZ:
            return 0;
        case TIOCGPGRP:
            memcpy(reinterpret_cast<void*>(arg), &g_foreground_pgrp, sizeof(i32));
            return 0;
        case TIOCSPGRP:
            memcpy(&g_foreground_pgrp, reinterpret_cast<void*>(arg), sizeof(i32));
            return 0;
        case FIONREAD: {
            i32 n = static_cast<i32>(g_ready_count);
            memcpy(reinterpret_cast<void*>(arg), &n, sizeof(n));
            return 0;
        }
        default:
            return -err::NOTTY;
        }
    }
};

Console* g_console = nullptr;

} // namespace

void input_char(char c) {
    bool wake = false;
    int send_signal = 0;
    {
        LockGuard guard(g_input_lock);
        if (c == '\r' && (g_termios.c_iflag & ICRNL))
            c = '\n';

        if ((g_termios.c_lflag & ISIG) &&
            (c == static_cast<char>(g_termios.c_cc[VINTR]) || c == static_cast<char>(g_termios.c_cc[VQUIT]))) {
            send_signal = c == static_cast<char>(g_termios.c_cc[VINTR]) ? signal::SIGINT : signal::SIGQUIT;
            echo(c == 3 ? "^C\n" : "^\\\n", 3);
            g_line_len = 0; // the partly typed line is discarded
            c = 0;
        }
        if (!c) {
            // Ctrl+C / Ctrl+\ handled above
        } else if (!(g_termios.c_lflag & ICANON)) {
            if (g_line_len < BUF_SIZE)
                g_line[g_line_len++] = c;
            echo(&c, 1);
            wake = true;
        } else if (c == static_cast<char>(g_termios.c_cc[VERASE]) || c == '\b') {
            if (g_line_len) {
                g_line_len--;
                echo("\b \b", 3);
            }
        } else if (c == static_cast<char>(g_termios.c_cc[VKILL])) {
            while (g_line_len) {
                g_line_len--;
                echo("\b \b", 3);
            }
        } else if (c == static_cast<char>(g_termios.c_cc[VEOF])) {
            push_ready(g_line, g_line_len);
            g_line_len = 0;
            if (g_ready_count == 0)
                push_ready("\x04", 1); // marks EOF for an empty line
            wake = true;
        } else {
            if (g_line_len < BUF_SIZE - 1)
                g_line[g_line_len++] = c;
            echo(&c, 1);
            if (c == '\n') {
                push_ready(g_line, g_line_len);
                g_line_len = 0;
                wake = true;
            }
        }
    }
    if (wake) {
        g_readers.wake_all();
        poll::notify();
    }
    if (send_signal)
        signal::send_to_group(g_foreground_pgrp, send_signal);
}

void set_foreground_group(int pgid) { g_foreground_pgrp = pgid; }

void init() {
    g_console = new Console;
    if (!serial::present())
        return;
    u8 vector = interrupts::allocate_vector();
    interrupts::set_handler(vector, serial_irq);
    ioapic::route_isa_irq(4, vector, this_cpu()->lapic_id);
    arch::outb(0x3F8 + 1, 0x01); // UART: interrupt on received data
}

vfs::Inode* console_inode() { return g_console; }

} // namespace tty
