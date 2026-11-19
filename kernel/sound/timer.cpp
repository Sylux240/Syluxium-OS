// /dev/snd/timer with PCM timers (Linux: snd_timer + snd_pcm_timer): an
// open timer file selects a PCM substream and then receives a tick event at
// every period boundary the stream passes. libasound's dmix/dsnoop (several
// programs sharing one sound card) wake up on these.

#include <core/log.hpp>
#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sound/sound.hpp>
#include <time/rtc.hpp>

namespace sound {

namespace {

using namespace abi;

enum : i32 { CLASS_NONE = -1, CLASS_PCM = 3 }; // SNDRV_TIMER_CLASS_*: SLAVE 0, GLOBAL 1, CARD 2, PCM 3
enum : i32 { EVENT_TICK = 1, EVENT_START = 2, EVENT_STOP = 3, EVENT_CONTINUE = 4, EVENT_PAUSE = 5 };
enum : u32 {
    NR_SELECT = 0x10, NR_INFO = 0x11, NR_PARAMS = 0x12, NR_STATUS = 0x14,
    NR_START = 0xa0, NR_STOP = 0xa1, NR_CONTINUE = 0xa2, NR_PAUSE = 0xa3, NR_TREAD64 = 0xa4,
};
constexpr u32 PSFLG_AUTO = 1;

struct TRead { // struct snd_timer_tread (64-bit time)
    i32 event;
    u8 pad1[4];
    i64 sec, nsec;
    u32 val;
    u8 pad2[4];
};
static_assert(sizeof(TRead) == 32);
struct Read { // struct snd_timer_read
    u32 resolution, ticks;
};

constexpr u32 QUEUE = 128;

struct Instance {
    Substream* s = nullptr;
    bool tread = false;
    bool running = false;
    u32 ticks = 1, acc = 0;     // report every `ticks` periods
    bool auto_restart = true;
    TRead queue[QUEUE];
    u32 head = 0, count = 0;
    u32 overrun = 0, lost = 0;
    Instance* next = nullptr;   // the substream's list
};

IrqSpinlock g_lock;          // instance lists and queues
sched::WaitQueue g_wait;     // readers

void push_locked(Instance* t, i32 event, u32 val) {
    if (t->count == QUEUE) {
        t->overrun++;
        return;
    }
    u64 ns = rtc::realtime_ns();
    TRead& e = t->queue[(t->head + t->count++) % QUEUE];
    e = TRead{};
    e.event = event;
    e.sec = static_cast<i64>(ns / 1'000'000'000);
    e.nsec = static_cast<i64>(ns % 1'000'000'000);
    e.val = val;
}

void unlink_locked(Instance* t) {
    if (!t->s)
        return;
    auto** link = reinterpret_cast<Instance**>(&t->s->timers);
    while (*link && *link != t)
        link = &(*link)->next;
    if (*link)
        *link = t->next;
    t->s = nullptr;
}

template <typename T>
long get_arg(T* out, u64 arg) { return uaccess::copy_from(out, reinterpret_cast<const void*>(arg), sizeof(T)); }
template <typename T>
long put_arg(u64 arg, const T& v) { return uaccess::copy_to(reinterpret_cast<void*>(arg), &v, sizeof(T)); }

// The substream a PCM timer id names (Linux's snd_pcm_timer_init):
// device = PCM device, subdevice = substream number << 1 | direction.
Substream* substream_for(const TimerId& id) {
    if (id.dev_class != CLASS_PCM)
        return nullptr;
    Card* card = card_by_number(id.card);
    if (!card || id.device < 0 || id.subdevice < 0)
        return nullptr;
    u32 pcm = static_cast<u32>(id.device), dir = static_cast<u32>(id.subdevice) & 1;
    if (pcm >= card->pcm_count || (id.subdevice >> 1) != 0)
        return nullptr;
    return card->pcms[pcm]->streams[dir];
}

class TimerNode : public vfs::Inode {
public:
    TimerNode() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0660;
        gid = vfs::GID_AUDIO;
        rdev = (116u << 8) | 33;
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override { f->private_data = new Instance; }
    void on_release(vfs::File* f) override {
        auto* t = static_cast<Instance*>(f->private_data);
        {
            LockGuard guard(g_lock);
            unlink_locked(t);
        }
        delete t;
    }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        auto* t = static_cast<Instance*>(f->private_data);
        usize unit = t->tread ? sizeof(TRead) : sizeof(Read);
        if (count < unit)
            return -err::INVAL;
        for (;;) {
            g_lock.lock();
            if (t->count) {
                usize n = min<usize>(min<usize>(count / unit, t->count), 8);
                u8 tmp[8 * sizeof(TRead)];
                for (usize i = 0; i < n; i++) {
                    TRead& e = t->queue[t->head];
                    if (t->tread) {
                        memcpy(tmp + i * unit, &e, unit);
                    } else {
                        Read r{1000000, e.val}; // resolution in ns (nominal)
                        memcpy(tmp + i * unit, &r, unit);
                    }
                    t->head = (t->head + 1) % QUEUE;
                }
                t->count -= static_cast<u32>(n);
                g_lock.unlock();
                if (long r = uaccess::copy_to(buf, tmp, n * unit); r < 0)
                    return r;
                return static_cast<isize>(n * unit);
            }
            g_lock.unlock();
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            g_wait.wait_until([&] { return t->count > 0 || signal::interrupted(); });
        }
    }

    u16 poll(vfs::File* f) override {
        auto* t = static_cast<Instance*>(f->private_data);
        LockGuard guard(g_lock);
        return t->count ? (poll::POLLIN | poll::POLLRDNORM) : 0;
    }

    long ioctl_file(vfs::File* f, u64 req, u64 arg) override {
        auto* t = static_cast<Instance*>(f->private_data);
        if (ioc_type(req) != 'T')
            return -err::NOTTY;
        switch (ioc_nr(req)) {
        case TIMER_PVERSION:
            return put_arg<i32>(arg, TIMER_VERSION);
        case TIMER_NEXT_DEVICE: {
            TimerId id;
            if (long r = get_arg(&id, arg); r < 0)
                return r;
            id = TimerId{CLASS_NONE, 0, -1, -1, -1}; // no global timers to enumerate
            return put_arg(arg, id);
        }
        case TIMER_TREAD:
        case NR_TREAD64: {
            i32 on;
            if (long r = get_arg(&on, arg); r < 0)
                return r;
            LockGuard guard(g_lock);
            t->tread = on != 0;
            t->head = t->count = 0;
            return 0;
        }
        case NR_SELECT: {
            TimerId id;
            if (long r = get_arg(&id, arg); r < 0)
                return r;
            Substream* s = substream_for(id);
            if (!s) {
                return -err::NODEV;
            }
            LockGuard guard(g_lock);
            unlink_locked(t);
            t->s = s;
            t->next = static_cast<Instance*>(s->timers);
            s->timers = t;
            t->running = false;
            t->head = t->count = 0;
            return 0;
        }
        case NR_INFO: { // struct snd_timer_info
            struct {
                u32 flags;
                i32 card;
                char id[64], name[80];
                u64 reserved0, resolution;
                u8 reserved[64];
            } info{};
            if (!t->s)
                return -snderr::BADFD;
            info.card = t->s->card->number;
            snprintf(info.id, sizeof(info.id), "PCM");
            snprintf(info.name, sizeof(info.name), "%s PCM timer", t->s->card->name);
            u64 rate = t->s->cfg.rate ? t->s->cfg.rate : 48000;
            info.resolution = t->s->cfg.period_size ? t->s->cfg.period_size * 1'000'000'000ull / rate : 1'000'000;
            return put_arg(arg, info);
        }
        case NR_PARAMS: { // struct snd_timer_params
            struct {
                u32 flags, ticks, queue_size, reserved0, filter;
                u8 reserved[60];
            } p;
            if (long r = get_arg(&p, arg); r < 0)
                return r;
            LockGuard guard(g_lock);
            t->ticks = p.ticks ? p.ticks : 1;
            t->auto_restart = p.flags & PSFLG_AUTO;
            t->acc = 0;
            return 0;
        }
        case NR_STATUS: { // struct snd_timer_status (64-bit time)
            struct {
                i64 sec, nsec;
                u32 resolution, lost, overrun, queue;
                u8 reserved[64];
            } st{};
            LockGuard guard(g_lock);
            u64 ns = rtc::realtime_ns();
            st.sec = static_cast<i64>(ns / 1'000'000'000);
            st.nsec = static_cast<i64>(ns % 1'000'000'000);
            st.resolution = 1'000'000;
            st.lost = t->lost;
            st.overrun = t->overrun;
            st.queue = t->count;
            return put_arg(arg, st);
        }
        case NR_START:
        case NR_CONTINUE: {
            LockGuard guard(g_lock);
            if (!t->s)
                return -snderr::BADFD;
            t->running = true;
            t->acc = 0;
            return 0;
        }
        case NR_STOP:
        case NR_PAUSE: {
            LockGuard guard(g_lock);
            t->running = false;
            return 0;
        }
        default:
            return -err::NOTTY;
        }
    }
};

} // namespace

namespace internal {

void register_timer_node() { vfs::add_node("/dev/snd/timer", new TimerNode); }

// The stream passed `periods` period boundaries.
void timer_ticks(Substream* s, u64 periods) {
    if (!__atomic_load_n(&s->timers, __ATOMIC_ACQUIRE) || !periods)
        return;
    bool any = false;
    {
        LockGuard guard(g_lock);
        for (auto* t = static_cast<Instance*>(s->timers); t; t = t->next) {
            if (!t->running)
                continue;
            t->acc += static_cast<u32>(periods);
            if (t->acc >= t->ticks) {
                push_locked(t, EVENT_TICK, t->acc / t->ticks);
                t->acc %= t->ticks;
                any = true;
                if (!t->auto_restart)
                    t->running = false;
            }
        }
    }
    if (any) {
        g_wait.wake_all();
        poll::notify();
    }
}

} // namespace internal

} // namespace sound
