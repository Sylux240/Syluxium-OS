// virtio-sound (virtio 1.2, device id 25): the paravirtual sound card of
// QEMU/KVM and crosvm.
//
// The device has no DMA ring of its own: audio moves in buffers the driver
// queues (txq for playback, rxq for capture), each holding one ALSA period.
// Like Linux's virtio_snd we queue every period of the ring buffer when the
// stream starts and requeue each one as the device hands it back, so the
// ring cycles exactly like a DMA engine's; the "hardware position" is the
// number of completed periods. Control requests (set params, prepare,
// start, stop, release) go over the control queue and may sleep, so
// start/stop requested from the (non-sleeping) trigger callback are carried
// out by the driver's worker thread.

#include <arch/x86_64/interrupts.hpp>
#include <core/log.hpp>
#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/sound/sound_drivers.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <sound/sound.hpp>
#include <time/clock.hpp>

namespace virtio_snd {

namespace {

constexpr u32 STATUS_ACK = 1, STATUS_DRIVER = 2, STATUS_DRIVER_OK = 4, STATUS_FEATURES_OK = 8;
constexpr u16 DESC_NEXT = 1, DESC_WRITE = 2;
constexpr u16 MAX_QUEUE = 256;

// Control request codes and status values.
constexpr u32 R_PCM_INFO = 0x0100, R_PCM_SET_PARAMS = 0x0101, R_PCM_PREPARE = 0x0102, R_PCM_RELEASE = 0x0103,
              R_PCM_START = 0x0104, R_PCM_STOP = 0x0105;
constexpr u32 S_OK = 0x8000;
constexpr u8 DIR_OUTPUT = 0, DIR_INPUT = 1;

struct PACKED PcmInfo {
    u32 hda_fn_nid;
    u32 features;
    u64 formats;
    u64 rates;
    u8 direction, channels_min, channels_max;
    u8 padding[5];
};
static_assert(sizeof(PcmInfo) == 32);

struct PACKED SetParams {
    u32 code, stream_id;
    u32 buffer_bytes, period_bytes, features;
    u8 channels, format, rate, padding;
};

struct Desc {
    u64 addr;
    u32 len;
    u16 flags, next;
};

struct Queue {
    u16 size = 0;
    u16 index = 0;
    Desc* desc = nullptr;
    volatile u16* avail = nullptr; // flags, idx, ring[size]
    volatile u8* used = nullptr;   // flags, idx, ring[size] of {u32 id, u32 len}
    u16 avail_idx = 0, used_seen = 0;
    volatile u16* notify = nullptr;

    u16 used_idx() const { return *reinterpret_cast<volatile u16*>(used + 2); }
    u32 used_id(u16 i) const { return *reinterpret_cast<volatile u32*>(used + 4 + (i % size) * 8); }
    void offer(u16 head) {
        avail[2 + avail_idx % size] = head;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        avail[1] = ++avail_idx;
    }
    void kick() { *notify = index; }
};

// virtio format / rate codes -> ALSA.
struct FormatMap {
    u8 virtio;
    int alsa;
};
constexpr FormatMap FORMATS[] = {
    {3, sound::abi::FORMAT_S8},       {4, sound::abi::FORMAT_U8},     {5, sound::abi::FORMAT_S16_LE},
    {15, sound::abi::FORMAT_S24_LE},  {17, sound::abi::FORMAT_S32_LE}, {19, sound::abi::FORMAT_FLOAT_LE},
};
constexpr u32 RATES[] = {5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 384000};

class Card;

struct Stream {
    Card* card = nullptr;
    u32 id = 0;          // device stream id
    bool capture = false;
    Queue* q = nullptr;
    sound::Substream* sub = nullptr;
    // Per-period request header (stream id) and device-written status, 16 bytes each.
    u8* meta = nullptr;
    u64 meta_phys = 0;
    u32 periods = 0, period_bytes = 0;
    u64 completed = 0;   // periods the device has finished (since prepare)
    u32 in_flight = 0;
    bool running = false;   // requeue completed periods
    bool want_start = false, want_stop = false;
    bool params_set = false;
    IrqSpinlock lock;       // queue + counters (pointer() reaps under the PCM lock)
};

class Card {
public:
    volatile u8* common = nullptr;
    volatile u8* device_cfg = nullptr;
    volatile u8* isr = nullptr;
    volatile u8* notify_base = nullptr;
    u32 notify_mult = 0;
    Queue ctlq, evq, txq, rxq;
    u8* ctl_buf = nullptr;
    u64 ctl_phys = 0;
    sched::Mutex ctl_lock;
    sched::Semaphore wake;
    Stream play, cap;
    sound::Card* card = nullptr;

    template <typename T> T c_rd(u32 off) { return *reinterpret_cast<volatile T*>(common + off); }
    template <typename T> void c_wr(u32 off, T v) { *reinterpret_cast<volatile T*>(common + off) = v; }

    bool setup_queue(Queue& q, u16 index);
    long control(const void* req, u32 req_len, void* resp, u32 resp_len);
    long simple(u32 code, u32 stream_id);
    void reap(Stream& st);
    void queue_all(Stream& st);
};

bool Card::setup_queue(Queue& q, u16 index) {
    c_wr<u16>(22, index); // queue_select
    u16 size = c_rd<u16>(24);
    if (!size)
        return false;
    if (size > MAX_QUEUE)
        size = MAX_QUEUE;
    c_wr<u16>(24, size);
    q.size = size;
    q.index = index;
    u64 d = pmm::alloc_pages(pmm::order_for_size(size * sizeof(Desc)), true), a = pmm::alloc_page(true),
        u = pmm::alloc_page(true);
    if (!d || !a || !u)
        return false;
    q.desc = mm::phys_to_virt<Desc>(d);
    q.avail = mm::phys_to_virt<u16>(a);
    q.used = mm::phys_to_virt<u8>(u);
    c_wr<u64>(32, d);
    c_wr<u64>(40, a);
    c_wr<u64>(48, u);
    c_wr<u16>(26, 0); // queue_msix_vector: entry 0 (ignored under plain MSI)
    q.notify = reinterpret_cast<volatile u16*>(notify_base + c_rd<u16>(30) * notify_mult);
    c_wr<u16>(28, 1); // queue_enable
    return true;
}

// One control request: [request (device-readable)] -> [response (device-writable)].
long Card::control(const void* req, u32 req_len, void* resp, u32 resp_len) {
    LockGuard guard(ctl_lock);
    if (req_len + resp_len > 4096)
        return -err::INVAL;
    memcpy(ctl_buf, req, req_len);
    memset(ctl_buf + 2048, 0, resp_len);
    ctlq.desc[0] = Desc{ctl_phys, req_len, DESC_NEXT, 1};
    ctlq.desc[1] = Desc{ctl_phys + 2048, resp_len, DESC_WRITE, 0};
    u16 before = ctlq.used_idx();
    ctlq.offer(0);
    ctlq.kick();
    u64 deadline = clock::now_ns() + 500'000'000;
    while (ctlq.used_idx() == before) {
        if (clock::now_ns() > deadline)
            return -err::TIMEDOUT;
        sched::yield();
    }
    ctlq.used_seen = ctlq.used_idx();
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    memcpy(resp, ctl_buf + 2048, resp_len);
    u32 status = *static_cast<u32*>(resp);
    return status == S_OK ? 0 : -err::IO;
}

long Card::simple(u32 code, u32 stream_id) {
    u32 req[2] = {code, stream_id};
    u32 resp = 0;
    return control(req, sizeof(req), &resp, sizeof(resp));
}

// Collect finished periods; requeue them while the stream runs (lock held).
void Card::reap(Stream& st) {
    Queue& q = *st.q;
    bool kicked = false;
    while (q.used_seen != q.used_idx()) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        u32 head = q.used_id(q.used_seen++);
        if (st.in_flight)
            st.in_flight--;
        st.completed++;
        if (st.running && head / 3 < st.periods) {
            q.offer(static_cast<u16>(head));
            st.in_flight++;
            kicked = true;
        }
    }
    if (kicked)
        q.kick();
}

// Queue every period: three descriptors each (header, audio, status).
void Card::queue_all(Stream& st) {
    Queue& q = *st.q;
    for (u32 i = 0; i < st.periods; i++) {
        u16 base = static_cast<u16>(i * 3);
        u8* m = st.meta + i * 16;
        *reinterpret_cast<u32*>(m) = st.id;
        u64 mp = st.meta_phys + i * 16;
        u64 data = st.sub->dma_phys + static_cast<u64>(i) * st.period_bytes;
        q.desc[base] = Desc{mp, 4, DESC_NEXT, static_cast<u16>(base + 1)};
        q.desc[base + 1] = Desc{data, st.period_bytes, static_cast<u16>(DESC_NEXT | (st.capture ? DESC_WRITE : 0)),
                                static_cast<u16>(base + 2)};
        q.desc[base + 2] = Desc{mp + 8, 8, DESC_WRITE, 0};
        q.offer(base);
        st.in_flight++;
    }
    q.kick();
}

// ---- PCM ops ----

Stream* stream_of(sound::Substream* s) { return static_cast<Stream*>(s->driver_data); }

u8 virtio_format(int alsa) {
    for (const auto& f : FORMATS)
        if (f.alsa == alsa)
            return f.virtio;
    return 5;
}

u8 virtio_rate(u32 rate) {
    for (u8 i = 0; i < array_size(RATES); i++)
        if (RATES[i] == rate)
            return i;
    return 7;
}

// Stop the device side and wait until it has returned every buffer. The
// spec only guarantees that on RELEASE ("complete all pending I/O"), so
// stop, release, then wait.
void quiesce(Stream& st) {
    Card* c = st.card;
    bool had_buffers;
    {
        LockGuard guard(st.lock);
        st.running = false;
        st.want_start = st.want_stop = false;
        had_buffers = st.in_flight > 0;
    }
    if (had_buffers)
        c->simple(R_PCM_STOP, st.id);
    if (st.params_set)
        c->simple(R_PCM_RELEASE, st.id);
    st.params_set = false;
    u64 deadline = clock::now_ns() + 1'000'000'000;
    for (;;) {
        {
            LockGuard guard(st.lock);
            c->reap(st);
            if (!st.in_flight)
                break;
        }
        if (clock::now_ns() > deadline) {
            klog::warn("virtio-snd: stream %u did not return its buffers", st.id);
            LockGuard guard(st.lock);
            st.in_flight = 0; // don't wait for them again
            break;
        }
        sched::sleep_ms(1);
    }
}

long op_prepare(sound::Substream* s) {
    Stream& st = *stream_of(s);
    Card* c = st.card;
    quiesce(st);
    const sound::PcmConfig& cfg = s->cfg;
    if (cfg.periods * 3 > st.q->size)
        return -err::INVAL;
    SetParams p{};
    p.code = R_PCM_SET_PARAMS;
    p.stream_id = st.id;
    p.buffer_bytes = static_cast<u32>(cfg.buffer_size * cfg.frame_bytes);
    p.period_bytes = static_cast<u32>(cfg.period_size * cfg.frame_bytes);
    p.channels = static_cast<u8>(cfg.channels);
    p.format = virtio_format(cfg.format);
    p.rate = virtio_rate(cfg.rate);
    u32 resp = 0;
    if (long r = c->control(&p, sizeof(p), &resp, sizeof(resp)); r < 0)
        return r;
    st.params_set = true;
    if (long r = c->simple(R_PCM_PREPARE, st.id); r < 0)
        return r;
    LockGuard guard(st.lock);
    st.periods = static_cast<u32>(cfg.periods);
    st.period_bytes = p.period_bytes;
    st.completed = 0;
    return 0;
}

long op_trigger(sound::Substream* s, int cmd) {
    Stream& st = *stream_of(s);
    {
        LockGuard guard(st.lock);
        if (cmd == sound::TRIGGER_START) {
            st.want_start = true;
            st.want_stop = false;
        } else if (cmd == sound::TRIGGER_STOP) {
            st.running = false; // stop requeueing right away
            st.want_stop = true;
            st.want_start = false;
        } else {
            return -err::INVAL; // no pause: virtio-snd can't tell how much of a returned buffer was played
        }
    }
    st.card->wake.signal();
    return 0;
}

u64 op_pointer(sound::Substream* s) {
    Stream& st = *stream_of(s);
    LockGuard guard(st.lock);
    st.card->reap(st);
    if (!st.periods)
        return 0;
    return (st.completed % st.periods) * s->cfg.period_size;
}

void op_hw_free(sound::Substream* s) { quiesce(*stream_of(s)); }

constexpr sound::PcmOps g_ops{op_prepare, op_trigger, op_pointer, op_hw_free};

// ---- Worker: deferred start/stop, completion polling ----

Card* g_cards[4];
u32 g_card_count = 0;

void irq(interrupts::Frame*) {
    for (u32 i = 0; i < g_card_count; i++) {
        (void)*g_cards[i]->isr; // reading acknowledges
        g_cards[i]->wake.signal();
    }
}

void worker(void* arg) {
    auto* c = static_cast<Card*>(arg);
    for (;;) {
        bool active = c->play.running || c->cap.running;
        c->wake.wait_timeout(active ? 2'000'000 : 200'000'000);
        Stream* streams[] = {&c->play, &c->cap};
        for (Stream* st : streams) {
            if (!st->sub)
                continue;
            bool start, stop;
            u64 before;
            {
                LockGuard guard(st->lock);
                start = st->want_start;
                stop = st->want_stop;
                st->want_start = st->want_stop = false;
                if (start) {
                    st->running = true;
                    c->queue_all(*st);
                }
                before = st->completed;
                c->reap(*st);
            }
            if (start)
                c->simple(R_PCM_START, st->id);
            if (stop)
                c->simple(R_PCM_STOP, st->id);
            if (st->completed != before || st->running)
                sound::period_elapsed(st->sub);
        }
    }
}

sound::PcmHardware hardware_for(const PcmInfo& info, u16 queue_size) {
    sound::PcmHardware hw;
    for (const auto& f : FORMATS)
        if (info.formats & (1ull << f.virtio))
            hw.formats |= 1ull << f.alsa;
    for (u32 i = 0; i < array_size(RATES) && hw.rate_count < array_size(hw.rates); i++)
        if (info.rates & (1ull << i))
            hw.rates[hw.rate_count++] = RATES[i];
    hw.channels_min = info.channels_min ? info.channels_min : 1;
    hw.channels_max = info.channels_max ? info.channels_max : 2;
    hw.periods_min = 2;
    hw.periods_max = min<u32>(32, queue_size / 3);
    hw.period_bytes_min = 64;
    hw.info = sound::abi::INFO_INTERLEAVED | sound::abi::INFO_MMAP | sound::abi::INFO_MMAP_VALID |
              sound::abi::INFO_BLOCK_TRANSFER;
    return hw;
}

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && d->vendor == 0x1AF4 && d->device == 0x1059; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* c = new Card;
    for (u8 cap = pci::read8(*f, 0x34) & 0xFC; cap; cap = pci::read8(*f, cap + 1) & 0xFC) {
        if (pci::read8(*f, cap) != 0x09)
            continue;
        u8 type = pci::read8(*f, cap + 3);
        auto* base = static_cast<volatile u8*>(pci::map_bar(*f, pci::read8(*f, cap + 4)));
        if (!base)
            continue;
        u32 offset = pci::read32(*f, cap + 8);
        switch (type) {
        case 1: c->common = base + offset; break;
        case 2: c->notify_base = base + offset; c->notify_mult = pci::read32(*f, cap + 16); break;
        case 3: c->isr = base + offset; break;
        case 4: c->device_cfg = base + offset; break;
        default: break;
        }
    }
    if (!c->common || !c->notify_base || !c->isr || !c->device_cfg)
        return -err::NODEV;
    c->c_wr<u8>(20, 0); // reset
    while (c->c_rd<u8>(20))
        ;
    c->c_wr<u8>(20, STATUS_ACK | STATUS_DRIVER);
    c->c_wr<u32>(0, 1);
    if (!(c->c_rd<u32>(4) & 1))
        return -err::NODEV; // not VERSION_1
    c->c_wr<u32>(8, 0);
    c->c_wr<u32>(12, 0);
    c->c_wr<u32>(8, 1);
    c->c_wr<u32>(12, 1); // VERSION_1, nothing else
    c->c_wr<u8>(20, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(c->c_rd<u8>(20) & STATUS_FEATURES_OK))
        return -err::NODEV;
    c->c_wr<u16>(16, 0); // config change MSI-X vector: entry 0
    if (!c->setup_queue(c->ctlq, 0) || !c->setup_queue(c->evq, 1) || !c->setup_queue(c->txq, 2) ||
        !c->setup_queue(c->rxq, 3))
        return -err::NOMEM;
    c->ctl_phys = pmm::alloc_page(true);
    if (!c->ctl_phys)
        return -err::NOMEM;
    c->ctl_buf = mm::phys_to_virt<u8>(c->ctl_phys);

    u8 vector = interrupts::allocate_vector();
    bool msi = vector && g_card_count < array_size(g_cards) && pci::enable_msi(*f, vector);
    if (msi) {
        g_cards[g_card_count++] = c;
        interrupts::set_handler(vector, irq);
    }
    c->c_wr<u8>(20, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

    // Find the first output and the first input stream.
    u32 nstreams = *reinterpret_cast<volatile u32*>(c->device_cfg + 4);
    u32 query[4] = {R_PCM_INFO, 0, min<u32>(nstreams, 16), sizeof(PcmInfo)};
    static u8 resp[4 + 16 * sizeof(PcmInfo)];
    if (!nstreams || c->control(query, sizeof(query), resp, 4 + query[2] * sizeof(PcmInfo)) < 0) {
        klog::warn("virtio-snd %s: no PCM streams", d->bus_id);
        return -err::NODEV;
    }
    const PcmInfo* out = nullptr;
    const PcmInfo* in = nullptr;
    for (u32 i = 0; i < query[2]; i++) {
        auto* info = reinterpret_cast<const PcmInfo*>(resp + 4 + i * sizeof(PcmInfo));
        if (info->direction == DIR_OUTPUT && !out) {
            out = info;
            c->play.id = i;
        } else if (info->direction == DIR_INPUT && !in) {
            in = info;
            c->cap.id = i;
        }
    }

    c->card = sound::card_create("VirtIO", "virtio-snd", "VirtIO SoundCard", "VirtIO SoundCard at virtio PCI", d);
    if (!c->card)
        return -err::NOMEM;
    sound::Pcm* pcm = sound::pcm_create(c->card, "VirtIO PCM", out != nullptr, in != nullptr);
    if (!pcm)
        return -err::NOMEM;
    struct { Stream* st; const PcmInfo* info; Queue* q; int dir; } setups[] = {
        {&c->play, out, &c->txq, sound::abi::STREAM_PLAYBACK},
        {&c->cap, in, &c->rxq, sound::abi::STREAM_CAPTURE},
    };
    for (auto& s : setups) {
        if (!s.info)
            continue;
        s.st->card = c;
        s.st->capture = s.dir == sound::abi::STREAM_CAPTURE;
        s.st->q = s.q;
        s.st->sub = pcm->streams[s.dir];
        s.st->meta_phys = pmm::alloc_page(true);
        if (!s.st->meta_phys)
            return -err::NOMEM;
        s.st->meta = mm::phys_to_virt<u8>(s.st->meta_phys);
        if (!sound::pcm_set_ops(pcm, s.dir, hardware_for(*s.info, s.q->size), &g_ops, s.st))
            return -err::NOMEM;
    }
    sched::spawn("virtio-snd", worker, c, -1, true);
    snprintf(d->name, sizeof(d->name), "Virtio sound device");
    sound::card_register(c->card);
    klog::info("virtio-snd %s: %u stream(s)%s%s%s", d->bus_id, nstreams, out ? ", playback" : "",
               in ? ", capture" : "", msi ? ", MSI" : ", polled");
    return 0;
}

dev::Driver g_driver{"virtio_snd", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace virtio_snd
