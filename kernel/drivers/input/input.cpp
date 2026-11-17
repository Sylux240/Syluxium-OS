#include <drivers/input/input.hpp>
#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <proc/signal.hpp>
#include <sched/sync.hpp>
#include <time/rtc.hpp>

namespace input {

namespace {

// struct input_event on 64-bit Linux.
struct PACKED LinuxEvent {
    i64 sec, usec;
    u16 type, code;
    i32 value;
};
static_assert(sizeof(LinuxEvent) == 24);

constexpr usize CLIENT_QUEUE = 512;

// One reader (open file) of a device: its own event queue, as on Linux.
struct Client {
    LinuxEvent queue[CLIENT_QUEUE];
    usize head = 0, count = 0;
    Client* next = nullptr;
};

struct DeviceState {
    Device* dev;
    Client* clients = nullptr;
    sched::WaitQueue readers; // its lock protects `clients` and the queues
};

constexpr usize MAX_DEVICES = 32;
DeviceState g_states[MAX_DEVICES];
u32 g_count = 0;

DeviceState* state_of(const Device* d) { return d->index < g_count ? &g_states[d->index] : nullptr; }

// ioctl request decoding (asm-generic/ioctl.h).
constexpr u32 ioc_nr(u64 req) { return req & 0xFF; }
constexpr u32 ioc_type(u64 req) { return (req >> 8) & 0xFF; }
constexpr u32 ioc_size(u64 req) { return (req >> 16) & 0x3FFF; }

long copy_bits(u64 arg, const void* bits, usize bits_bytes, u32 size) {
    usize n = min<usize>(size, bits_bytes);
    memcpy(reinterpret_cast<void*>(arg), bits, n);
    if (size > n)
        memset(reinterpret_cast<u8*>(arg) + n, 0, size - n);
    return static_cast<long>(n);
}

class EvdevNode : public vfs::Inode {
public:
    explicit EvdevNode(DeviceState* st) : m_state(st) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0660;
        rdev = (13u << 8) | (64 + st->dev->index); // Linux: major 13, minor 64+N
        atime = mtime = ctime = vfs::now();
    }

    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override {
        auto* c = new Client;
        LockGuard guard(m_state->readers.lock());
        c->next = m_state->clients;
        m_state->clients = c;
        f->private_data = c;
    }

    void on_release(vfs::File* f) override {
        auto* c = static_cast<Client*>(f->private_data);
        {
            LockGuard guard(m_state->readers.lock());
            Client** link = &m_state->clients;
            while (*link && *link != c)
                link = &(*link)->next;
            if (*link)
                *link = c->next;
        }
        delete c;
    }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        auto* c = static_cast<Client*>(f->private_data);
        if (count < sizeof(LinuxEvent))
            return -err::INVAL;
        for (;;) {
            m_state->readers.lock().lock();
            if (c->count) {
                usize n = min(count / sizeof(LinuxEvent), c->count);
                auto* out = static_cast<LinuxEvent*>(buf);
                for (usize i = 0; i < n; i++) {
                    out[i] = c->queue[c->head];
                    c->head = (c->head + 1) % CLIENT_QUEUE;
                }
                c->count -= n;
                m_state->readers.lock().unlock();
                return static_cast<isize>(n * sizeof(LinuxEvent));
            }
            m_state->readers.lock().unlock();
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            m_state->readers.wait_until([&] { return c->count > 0 || signal::interrupted(); });
        }
    }

    isize write_file(vfs::File*, u64, const void*, usize count) override { return static_cast<isize>(count); }

    u16 poll(vfs::File* f) override {
        auto* c = static_cast<Client*>(f->private_data);
        LockGuard guard(m_state->readers.lock());
        return poll::POLLOUT | (c->count ? poll::POLLIN : 0);
    }

    long ioctl(u64 req, u64 arg) override {
        Device* d = m_state->dev;
        if (ioc_type(req) != 'E')
            return -err::NOTTY;
        u32 nr = ioc_nr(req), size = ioc_size(req);
        switch (nr) {
        case 0x01: { // EVIOCGVERSION
            i32 v = 0x010001;
            memcpy(reinterpret_cast<void*>(arg), &v, 4);
            return 0;
        }
        case 0x02: { // EVIOCGID
            u16 id[4] = {d->bustype, d->vendor, d->product, d->version};
            memcpy(reinterpret_cast<void*>(arg), id, 8);
            return 0;
        }
        case 0x06: { // EVIOCGNAME(len)
            usize n = min<usize>(size, strlen(d->name) + 1);
            memcpy(reinterpret_cast<void*>(arg), d->name, n);
            return static_cast<long>(n);
        }
        case 0x07: case 0x08: { // EVIOCGPHYS / EVIOCGUNIQ
            char s[32] = {};
            if (nr == 7)
                snprintf(s, sizeof(s), "syluxium/input%u", d->index);
            usize n = min<usize>(size, strlen(s) + 1);
            memcpy(reinterpret_cast<void*>(arg), s, n);
            return static_cast<long>(n);
        }
        case 0x09: // EVIOCGPROP
            return copy_bits(arg, "\0\0\0\0", 4, size);
        case 0x18: // EVIOCGKEY: currently pressed keys
            return copy_bits(arg, d->key_down, sizeof(d->key_down), size);
        case 0x19: case 0x1a: case 0x1b: // LED / SND / SW state: none
            return copy_bits(arg, "\0\0\0\0\0\0\0\0", 8, size);
        case 0x90: // EVIOCGRAB
        case 0xa0: // EVIOCSCLOCKID
            return 0;
        default:
            break;
        }
        if (nr >= 0x20 && nr < 0x40) { // EVIOCGBIT(type, len)
            u32 type = nr - 0x20;
            switch (type) {
            case 0: return copy_bits(arg, &d->ev_bits, 8, size);
            case EV_KEY: return copy_bits(arg, d->key_bits, sizeof(d->key_bits), size);
            case EV_REL: return copy_bits(arg, &d->rel_bits, 8, size);
            case EV_ABS: return copy_bits(arg, &d->abs_bits, 8, size);
            default: return copy_bits(arg, "\0\0\0\0\0\0\0\0", 8, size);
            }
        }
        if (nr >= 0x40 && nr < 0x80) { // EVIOCGABS(axis): struct input_absinfo
            u32 axis = nr - 0x40;
            i32 info[6] = {0, axis < 2 ? d->abs_min[axis] : 0, axis < 2 ? d->abs_max[axis] : 0, 0, 0, 0};
            memcpy(reinterpret_cast<void*>(arg), info, min<u32>(size, sizeof(info)));
            return 0;
        }
        return -err::INVAL;
    }

private:
    DeviceState* m_state;
};

} // namespace

void Device::report(u16 type, u16 code, i32 value) {
    if (type == EV_KEY && code <= KEY_MAX) {
        bool was_down = key_down[code / 64] & (1ull << (code % 64));
        if (value == 1 && was_down)
            value = 2; // key repeat
        if (value)
            key_down[code / 64] |= 1ull << (code % 64);
        else
            key_down[code / 64] &= ~(1ull << (code % 64));
    }
    DeviceState* st = state_of(this);
    if (!st)
        return;
    u64 ns = rtc::realtime_ns();
    LinuxEvent e{static_cast<i64>(ns / 1'000'000'000), static_cast<i64>(ns % 1'000'000'000 / 1000), type, code, value};
    {
        LockGuard guard(st->readers.lock());
        for (Client* c = st->clients; c; c = c->next) {
            if (c->count == CLIENT_QUEUE) { // reader too slow: drop the oldest event
                c->head = (c->head + 1) % CLIENT_QUEUE;
                c->count--;
            }
            c->queue[(c->head + c->count++) % CLIENT_QUEUE] = e;
        }
    }
    if (type == EV_SYN) {
        st->readers.wake_all();
        poll::notify();
    }
}

void register_device(Device* d) {
    if (g_count >= MAX_DEVICES)
        return;
    d->index = g_count;
    g_states[g_count].dev = d;
    d->ev_bits |= 1ull << EV_SYN;
    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%u", g_count);
    vfs::add_node(path, new EvdevNode(&g_states[g_count]));
    g_count++;
}

void init() { vfs::mkdir_p("/dev/input"); }

} // namespace input
