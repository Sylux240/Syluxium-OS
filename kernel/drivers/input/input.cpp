#include <core/watchdog.hpp>
#include <video/splash.hpp>
#include <drivers/input/input.hpp>
#include <drivers/input/input_sysfs.hpp>
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
    Device* dev;              // null once unplugged
    bool dead = false;
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

// Copy a bit array to the caller, zero-filling the rest of the length it
// gave (some programs rely on that) - except for switches: TDE's hardware
// daemon asks for EV_CNT (32) bytes of switch bits into an 8-byte array,
// and filling to the length it gave smashed its stack. Linux copies only
// its own array size there (8 bytes for switches).
long copy_bits(u64 arg, const void* bits, usize bits_bytes, u32 size, u32 fill_limit = 0xFFFFFFFF) {
    usize n = min<usize>(size, bits_bytes);
    memcpy(reinterpret_cast<void*>(arg), bits, n);
    usize fill = min<usize>(size, fill_limit);
    if (fill > n)
        memset(reinterpret_cast<u8*>(arg) + n, 0, fill - n);
    return static_cast<long>(n);
}

class EvdevNode : public vfs::Inode {
public:
    explicit EvdevNode(DeviceState* st) : m_state(st) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0660;
        gid = vfs::GID_INPUT;
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
            if (!c->count && m_state->dead) {
                m_state->readers.lock().unlock();
                return -err::NODEV; // unplugged
            }
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
            m_state->readers.wait_until([&] { return c->count > 0 || m_state->dead || signal::interrupted(); });
        }
    }

    isize write_file(vfs::File*, u64, const void*, usize count) override { return static_cast<isize>(count); }

    u16 poll(vfs::File* f) override {
        auto* c = static_cast<Client*>(f->private_data);
        LockGuard guard(m_state->readers.lock());
        if (m_state->dead)
            return poll::POLLIN | poll::POLLERR | poll::POLLHUP;
        return poll::POLLOUT | (c->count ? poll::POLLIN : 0);
    }

    long ioctl(u64 req, u64 arg) override {
        if (ioc_type(req) != 'E')
            return -err::NOTTY;
        // Held across the ioctl so the device can't be unplugged (and freed)
        // under us. sys_ioctl already faulted the user buffer in: no sleeping.
        LockGuard guard(m_state->readers.lock());
        Device* d = m_state->dev;
        if (!d)
            return -err::NODEV;
        u32 nr = ioc_nr(req), size = ioc_size(req);
        switch (nr) {
        case 0x01: { // EVIOCGVERSION
            i32 v = 0x010001;
            memcpy(reinterpret_cast<void*>(arg), &v, 4);
            return 0;
        }
        case 0x03: { // EVIOCGREP / EVIOCSREP: key repeat delay and period (ms)
            if ((req >> 30) == 1) // _IOW: set (repeat is generated by X/libinput, accept it)
                return 0;
            u32 rep[2] = {250, 33};
            memcpy(reinterpret_cast<void*>(arg), rep, sizeof(rep));
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
        { // Linux keeps the properties in one long: 8 bytes, not our 4
            u64 props = d->props;
            return copy_bits(arg, &props, sizeof(props), size);
        }
        case 0x18: // EVIOCGKEY: currently pressed keys
            return copy_bits(arg, d->key_down, sizeof(d->key_down), size);
        case 0x1b: // EVIOCGSW: switch state
            return copy_bits(arg, &d->sw_state, sizeof(d->sw_state), size, 8);
        case 0x19: case 0x1a: // LED / SND state: none
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
            case EV_MSC: return copy_bits(arg, &d->msc_bits, 8, size);
            case EV_SW: return copy_bits(arg, &d->sw_bits, 8, size, 8);
            default: { // FF and the rest: none (FF_CNT bits = 16 bytes)
                static const u64 none[2] = {};
                return copy_bits(arg, none, sizeof(none), size);
            }
            }
        }
        if (nr >= 0x40 && nr < 0x80) { // EVIOCGABS(axis): struct input_absinfo
            u32 axis = nr - 0x40; // < 64 = ABS_MAX + 1
            // struct input_absinfo: value, minimum, maximum, fuzz, flat, resolution
            i32 info[6] = {d->abs_value[axis], d->abs_min[axis], d->abs_max[axis], 0, d->abs_flat[axis],
                           d->abs_res[axis]};
            memcpy(reinterpret_cast<void*>(arg), info, min<u32>(size, sizeof(info)));
            return 0;
        }
        return -err::INVAL;
    }

private:
    DeviceState* m_state;
};

} // namespace

// event0 / event1: every keyboard and every mouse combined (like Linux's
// /dev/input/mice), registered first so their numbers never change. The X
// server (no udev hot-plug here) opens these two and so sees every
// keyboard and mouse, built in or plugged in later.
Device g_all_keyboards, g_all_pointers;

void mirror(Device* d, u16 type, u16 code, i32 value) {
    if (d == &g_all_keyboards || d == &g_all_pointers)
        return;
    if (type == EV_KEY && code > 0 && code < BTN_MISC) {
        g_all_keyboards.report(type, code, value ? 1 : 0); // its own repeat tracking
        d->mirror_pending |= 1;
    } else if ((type == EV_KEY && code >= BTN_LEFT && code <= BTN_EXTRA && (d->ev_bits & (1ull << EV_REL))) ||
               type == EV_REL) {
        g_all_pointers.report(type, code, value);
        d->mirror_pending |= 2;
    } else if (type == EV_SYN && d->mirror_pending) {
        if (d->mirror_pending & 1)
            g_all_keyboards.sync();
        if (d->mirror_pending & 2)
            g_all_pointers.sync();
        d->mirror_pending = 0;
    }
}

void Device::report(u16 type, u16 code, i32 value) {
    if (type == EV_KEY && code == 1 /* KEY_ESC */ && value == 1)
        splash::request_verbose();
    // Alt+SysRq(PrintScreen)+W, as on Linux: dump every thread's state into
    // the boot log and save it, for a desktop that froze but whose kernel
    // still runs.
    if (type == EV_KEY && code == 17 /* KEY_W */ && value == 1 &&
        (key_down[99 / 64] & (1ull << (99 % 64))) /* KEY_SYSRQ */ &&
        (key_down[0] & ((1ull << 56) /* KEY_LEFTALT */)) | (key_down[100 / 64] & (1ull << (100 % 64))))
        watchdog::request_thread_dump();
    mirror(this, type, code, value);
    if (type == EV_KEY && code <= KEY_MAX) {
        bool was_down = key_down[code / 64] & (1ull << (code % 64));
        if (value == 1 && was_down)
            value = 2; // key repeat
        if (value)
            key_down[code / 64] |= 1ull << (code % 64);
        else
            key_down[code / 64] &= ~(1ull << (code % 64));
    } else if (type == EV_ABS && code <= ABS_MAX) {
        abs_value[code] = value;
    } else if (type == EV_SW && code < 64) {
        if (value)
            sw_state |= 1ull << code;
        else
            sw_state &= ~(1ull << code);
    }
    DeviceState* st = state_of(this);
    if (!st || st->dev != this)
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
    // Reuse the lowest eventN of an unplugged device nobody has open any
    // more (as Linux does); otherwise take a new one.
    u32 slot = g_count;
    for (u32 i = 0; i < g_count; i++) {
        LockGuard guard(g_states[i].readers.lock());
        if (g_states[i].dead && !g_states[i].clients) {
            slot = i;
            break;
        }
    }
    if (slot >= MAX_DEVICES)
        return;
    d->index = slot;
    d->ev_bits |= 1ull << EV_SYN;
    {
        LockGuard guard(g_states[slot].readers.lock());
        g_states[slot].dev = d;
        g_states[slot].dead = false;
    }
    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%u", slot);
    vfs::add_node(path, new EvdevNode(&g_states[slot]));
    if (slot == g_count)
        g_count++;
    sysfs::added(slot);
}

void unregister_device(Device* d) {
    DeviceState* st = state_of(d);
    if (!st || st->dev != d)
        return;
    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%u", d->index);
    vfs::remove_node(path);
    sysfs::removed(d->index);
    {
        LockGuard guard(st->readers.lock());
        st->dev = nullptr;
        st->dead = true;
    }
    st->readers.wake_all();
    poll::notify();
}

bool sysfs::snapshot(u32 slot, Device* out, bool* combined) {
    if (slot >= g_count)
        return false;
    LockGuard guard(g_states[slot].readers.lock());
    Device* d = g_states[slot].dev;
    if (!d)
        return false;
    memcpy(static_cast<void*>(out), d, sizeof(Device));
    *combined = d == &g_all_keyboards || d == &g_all_pointers;
    return true;
}

void init() {
    vfs::mkdir_p("/dev/input");
    sysfs::init();
    auto name = [](Device& d, const char* n) {
        for (usize i = 0; n[i] && i < sizeof(d.name) - 1; i++)
            d.name[i] = n[i];
    };
    name(g_all_keyboards, "Syluxium combined keyboards");
    g_all_keyboards.bustype = BUS_VIRTUAL;
    g_all_keyboards.ev_bits = (1ull << EV_KEY) | (1ull << EV_REP);
    for (u16 k = 1; k < BTN_MISC; k++)
        g_all_keyboards.set_key(k);
    register_device(&g_all_keyboards); // event0
    name(g_all_pointers, "Syluxium combined mice");
    g_all_pointers.bustype = BUS_VIRTUAL;
    g_all_pointers.ev_bits = (1ull << EV_KEY) | (1ull << EV_REL);
    g_all_pointers.rel_bits = (1ull << REL_X) | (1ull << REL_Y) | (1ull << REL_WHEEL) | (1ull << REL_HWHEEL);
    for (u16 b = BTN_LEFT; b <= BTN_EXTRA; b++)
        g_all_pointers.set_key(b);
    g_all_pointers.props = 1u << INPUT_PROP_POINTER;
    register_device(&g_all_pointers); // event1
}

} // namespace input
