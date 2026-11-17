// Sound core: the card registry, device nodes, /proc/asound and
// /sys/class/sound, and the position-polling thread.

#include <core/log.hpp>
#include <fs/genfs.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <sched/sched.hpp>
#include <sound/sound.hpp>

namespace sound {

namespace {

using genfs::GeneratedFile;
using genfs::TextBuffer;
using namespace abi;

constexpr u32 MAX_CARDS = 8;
constexpr u32 DEFAULT_DMA_BYTES = 512 * KiB; // per stream; ~1.4 s of 48 kHz stereo S16

Card* g_cards[MAX_CARDS];
u32 g_card_count = 0;
int g_next_number = 0;
sched::Semaphore g_kick;
bool g_thread_started = false;

const char* state_name(int st) {
    static const char* const names[] = {"OPEN", "SETUP", "PREPARED", "RUNNING", "XRUN",
                                        "DRAINING", "PAUSED", "SUSPENDED", "DISCONNECTED"};
    return st >= 0 && st <= STATE_DISCONNECTED ? names[st] : "?";
}

const char* access_name(int a) {
    static const char* const names[] = {"MMAP_INTERLEAVED", "MMAP_NONINTERLEAVED", "MMAP_COMPLEX",
                                        "RW_INTERLEAVED", "RW_NONINTERLEAVED"};
    return a >= 0 && a <= ACCESS_RW_NONINTERLEAVED ? names[a] : "?";
}

// Position polling: running streams are checked every few milliseconds, so
// playback keeps going (and xruns are detected) even if a device's period
// interrupts don't arrive. Interrupts still give the low-latency wakeups.
void timer_thread(void*) {
    for (;;) {
        bool running = false;
        u32 count = __atomic_load_n(&g_card_count, __ATOMIC_ACQUIRE);
        for (u32 c = 0; c < count; c++) {
            Card* card = g_cards[c];
            for (u32 p = 0; p < card->pcm_count; p++)
                for (Substream* s : card->pcms[p]->streams)
                    running |= internal::update_running(s);
        }
        if (running) {
            sched::sleep_ms(4);
            while (g_kick.try_wait()) {
            }
        } else {
            g_kick.wait();
        }
    }
}

// ---- /proc/asound ----

bool gen_version(TextBuffer& out, const void*) {
    out.append("Advanced Linux Sound Architecture Driver Version k6.12.0-syluxium.\n");
    return true;
}

bool gen_cards(TextBuffer& out, const void*) {
    if (g_card_count == 0)
        out.append("--- no soundcards ---\n");
    for (u32 i = 0; i < g_card_count; i++) {
        Card* c = g_cards[i];
        out.printf("%2d [%-15s]: %s - %s\n                      %s\n", c->number, c->id, c->driver, c->name,
                   c->longname);
    }
    return true;
}

bool gen_pcm(TextBuffer& out, const void*) {
    for (u32 i = 0; i < g_card_count; i++) {
        Card* c = g_cards[i];
        for (u32 p = 0; p < c->pcm_count; p++) {
            Pcm* pcm = c->pcms[p];
            out.printf("%02d-%02u: %s : %s", c->number, pcm->device, pcm->id, pcm->name);
            if (pcm->streams[0])
                out.append(" : playback 1");
            if (pcm->streams[1])
                out.append(" : capture 1");
            out.append("\n");
        }
    }
    return true;
}

bool gen_devices(TextBuffer& out, const void*) {
    out.append("  1:        : sequencer\n");
    for (u32 i = 0; i < g_card_count; i++) {
        Card* c = g_cards[i];
        for (u32 p = 0; p < c->pcm_count; p++) {
            Pcm* pcm = c->pcms[p];
            if (pcm->streams[0])
                out.printf("%3d: [%2d-%2u]: digital audio playback\n", c->number * 32 + 16 + static_cast<int>(pcm->device),
                           c->number, pcm->device);
            if (pcm->streams[1])
                out.printf("%3d: [%2d-%2u]: digital audio capture\n", c->number * 32 + 24 + static_cast<int>(pcm->device),
                           c->number, pcm->device);
        }
        out.printf("%3d: [%2d]   : control\n", c->number * 32, c->number);
    }
    out.append(" 33:        : timer\n");
    return true;
}

bool gen_card_id(TextBuffer& out, const void* ctx) {
    out.printf("%s\n", static_cast<const Card*>(ctx)->id);
    return true;
}

bool gen_stream_info(TextBuffer& out, const void* ctx) {
    auto* s = static_cast<const Substream*>(ctx);
    out.printf("card: %d\ndevice: %u\nsubdevice: 0\nstream: %s\nid: %s\nname: %s\nsubname: subdevice #0\n"
               "class: 0\nsubclass: 0\nsubdevices_count: 1\nsubdevices_avail: %d\n",
               s->card->number, s->pcm->device, s->stream == STREAM_PLAYBACK ? "PLAYBACK" : "CAPTURE", s->pcm->id,
               s->pcm->name, s->open ? 0 : 1);
    return true;
}

bool gen_hw_params(TextBuffer& out, const void* ctx) {
    auto* s = static_cast<const Substream*>(ctx);
    if (!s->open || !s->configured) {
        out.append("closed\n");
        return true;
    }
    const PcmConfig& c = s->cfg;
    out.printf("access: %s\nformat: %s\nsubformat: STD\nchannels: %u\nrate: %u (%u/1)\nperiod_size: %lu\n"
               "buffer_size: %lu\n",
               access_name(c.access), format_name(c.format), c.channels, c.rate, c.rate, c.period_size, c.buffer_size);
    return true;
}

bool gen_sw_params(TextBuffer& out, const void* ctx) {
    auto* s = static_cast<const Substream*>(ctx);
    if (!s->open || !s->configured) {
        out.append("closed\n");
        return true;
    }
    out.printf("tstamp_mode: %s\nperiod_step: 1\navail_min: %lu\nstart_threshold: %lu\nstop_threshold: %lu\n"
               "silence_threshold: %lu\nsilence_size: %lu\nboundary: %lu\n",
               s->tstamp_mode ? "ENABLE" : "NONE", s->control->avail_min, s->start_threshold, s->stop_threshold,
               s->silence_threshold, s->silence_size, s->boundary);
    return true;
}

bool gen_status(TextBuffer& out, const void* ctx) {
    auto* s = static_cast<const Substream*>(ctx);
    if (!s->open) {
        out.append("closed\n");
        return true;
    }
    out.printf("state: %s\nowner_pid   : 0\nhw_ptr      : %lu\nappl_ptr    : %lu\nxruns       : %lu\n",
               state_name(s->status->state), s->status->hw_ptr, s->control->appl_ptr, s->xruns);
    return true;
}

void add_file(const char* path, GeneratedFile::Generator gen, const void* ctx) {
    vfs::add_node(path, new GeneratedFile(gen, ctx));
}

void make_symlink(const char* dir, const char* name, const char* target) {
    vfs::Inode* d;
    if (vfs::resolve(dir, nullptr, true, &d) == 0 && !d->lookup(name, strlen(name)))
        d->create(name, strlen(name), vfs::S_IFLNK | 0777, target, nullptr);
}

} // namespace

void init() {
    vfs::mkdir_p("/dev/snd");
    internal::register_timer_node();
    vfs::mkdir_p("/proc/asound");
    add_file("/proc/asound/version", gen_version, nullptr);
    add_file("/proc/asound/cards", gen_cards, nullptr);
    add_file("/proc/asound/pcm", gen_pcm, nullptr);
    add_file("/proc/asound/devices", gen_devices, nullptr);
    genfs::static_file("/proc/asound/timers", "G0: system timer : 1000.000us (10000000 ticks)\n");
    vfs::mkdir_p("/sys/class/sound/timer");
    genfs::static_file("/sys/class/sound/timer/dev", "116:33\n");
}

Card* card_create(const char* id, const char* driver, const char* name, const char* longname, dev::Device* d) {
    if (g_next_number >= static_cast<int>(MAX_CARDS))
        return nullptr;
    auto* c = new Card;
    c->number = g_next_number++;
    // Card ids must be unique ("hw:CARD=<id>"); add the number on a clash, like Linux.
    copy_name(c->id, id, sizeof(c->id));
    for (u32 i = 0; i < g_card_count; i++)
        if (strcmp(g_cards[i]->id, c->id) == 0)
            snprintf(c->id, sizeof(c->id), "%.12s_%d", id, c->number);
    copy_name(c->driver, driver, sizeof(c->driver));
    copy_name(c->name, name, sizeof(c->name));
    copy_name(c->longname, longname, sizeof(c->longname));
    copy_name(c->mixername, name, sizeof(c->mixername));
    c->device = d;
    return c;
}

Pcm* pcm_create(Card* card, const char* name, bool playback, bool capture) {
    if (card->pcm_count >= MAX_PCMS)
        return nullptr;
    auto* p = new Pcm;
    p->card = card;
    p->device = card->pcm_count;
    copy_name(p->id, name, sizeof(p->id));
    copy_name(p->name, name, sizeof(p->name));
    for (int dir = 0; dir < 2; dir++) {
        if (!(dir == 0 ? playback : capture))
            continue;
        auto* s = new Substream;
        s->card = card;
        s->pcm = p;
        s->stream = dir;
        p->streams[dir] = s;
    }
    card->pcms[card->pcm_count++] = p;
    return p;
}

bool pcm_set_ops(Pcm* pcm, int stream, const PcmHardware& hw, const PcmOps* ops, void* driver_data) {
    Substream* s = pcm->streams[stream];
    if (!s)
        return false;
    s->hw = hw;
    s->ops = ops;
    s->driver_data = driver_data;
    u32 bytes = hw.buffer_bytes_max ? min<u32>(hw.buffer_bytes_max, DEFAULT_DMA_BYTES) : DEFAULT_DMA_BYTES;
    unsigned order = pmm::order_for_size(bytes);
    s->dma_phys = pmm::alloc_pages(order, true);
    s->status_phys = pmm::alloc_page(true);
    s->control_phys = pmm::alloc_page(true);
    if (!s->dma_phys || !s->status_phys || !s->control_phys)
        return false;
    s->dma_area = mm::phys_to_virt<u8>(s->dma_phys);
    s->dma_bytes = bytes;
    s->status = mm::phys_to_virt<MmapStatus>(s->status_phys);
    s->control = mm::phys_to_virt<MmapControl>(s->control_phys);
    s->status->state = STATE_OPEN;
    return true;
}

long card_register(Card* card) {
    if (card->registered)
        return 0;
    char path[64], dir[48];
    snprintf(path, sizeof(path), "/dev/snd/controlC%d", card->number);
    internal::register_control_node(card, path);

    snprintf(dir, sizeof(dir), "/proc/asound/card%d", card->number);
    vfs::mkdir_p(dir);
    snprintf(path, sizeof(path), "%s/id", dir);
    add_file(path, gen_card_id, card);
    make_symlink("/proc/asound", card->id, dir + strlen("/proc/asound/"));

    snprintf(path, sizeof(path), "/sys/class/sound/card%d", card->number);
    vfs::mkdir_p(path);
    snprintf(path, sizeof(path), "/sys/class/sound/card%d/id", card->number);
    char text[32];
    snprintf(text, sizeof(text), "%s\n", card->id);
    genfs::static_file(path, text);
    snprintf(path, sizeof(path), "/sys/class/sound/card%d/number", card->number);
    snprintf(text, sizeof(text), "%d\n", card->number);
    genfs::static_file(path, text);
    snprintf(path, sizeof(path), "/sys/class/sound/controlC%d", card->number);
    vfs::mkdir_p(path);
    snprintf(path, sizeof(path), "/sys/class/sound/controlC%d/dev", card->number);
    snprintf(text, sizeof(text), "116:%d\n", card->number * 32);
    genfs::static_file(path, text);

    for (u32 p = 0; p < card->pcm_count; p++) {
        Pcm* pcm = card->pcms[p];
        for (int dir_i = 0; dir_i < 2; dir_i++) {
            Substream* s = pcm->streams[dir_i];
            if (!s || !s->ops)
                continue;
            char c = dir_i == 0 ? 'p' : 'c';
            snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%u%c", card->number, pcm->device, c);
            internal::register_pcm_node(s, path);

            char sdir[64];
            snprintf(sdir, sizeof(sdir), "%s/pcm%u%c/sub0", dir, pcm->device, c);
            vfs::mkdir_p(sdir);
            snprintf(path, sizeof(path), "%s/pcm%u%c/info", dir, pcm->device, c);
            add_file(path, gen_stream_info, s);
            snprintf(path, sizeof(path), "%s/info", sdir);
            add_file(path, gen_stream_info, s);
            snprintf(path, sizeof(path), "%s/hw_params", sdir);
            add_file(path, gen_hw_params, s);
            snprintf(path, sizeof(path), "%s/sw_params", sdir);
            add_file(path, gen_sw_params, s);
            snprintf(path, sizeof(path), "%s/status", sdir);
            add_file(path, gen_status, s);

            snprintf(path, sizeof(path), "/sys/class/sound/pcmC%dD%u%c", card->number, pcm->device, c);
            vfs::mkdir_p(path);
            snprintf(path, sizeof(path), "/sys/class/sound/pcmC%dD%u%c/dev", card->number, pcm->device, c);
            snprintf(text, sizeof(text), "116:%u\n", card->number * 32 + (dir_i == 0 ? 16 : 24) + pcm->device);
            genfs::static_file(path, text);
        }
    }
    g_cards[g_card_count] = card;
    __atomic_store_n(&g_card_count, g_card_count + 1, __ATOMIC_RELEASE); // the timer thread reads it unlocked
    card->registered = true;
    if (!g_thread_started) {
        g_thread_started = true;
        sched::spawn("sound-timer", timer_thread, nullptr, -1, true);
    }
    klog::ok("Sound card %d: %s (%u PCM device%s, %u mixer controls)", card->number, card->longname, card->pcm_count,
             card->pcm_count == 1 ? "" : "s", card->control_count);
    return 0;
}

void card_proc_file(Card* card, const char* name, bool (*gen)(genfs::TextBuffer&, const void*), const void* ctx) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/asound/card%d/%s", card->number, name);
    add_file(path, gen, ctx);
}

Card* card_by_number(int n) {
    for (u32 i = 0; i < g_card_count; i++)
        if (g_cards[i]->number == n)
            return g_cards[i];
    return nullptr;
}
u32 card_count() { return g_card_count; }

namespace internal {
void timer_kick() { g_kick.signal(); }
} // namespace internal

} // namespace sound
