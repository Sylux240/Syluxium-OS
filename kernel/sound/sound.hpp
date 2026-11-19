#pragma once

#include <dev/device.hpp>
#include <lib/types.hpp>
#include <sched/sync.hpp>
#include <sound/asound.hpp>

// The sound core: an ALSA-compatible kernel interface.
//
// Drivers (HD Audio, virtio-sound, ...) describe a sound card made of PCM
// streams and mixer controls; the core turns that into Linux's device nodes
// (/dev/snd/controlC<n>, pcmC<n>D<d>p/c, timer), /proc/asound and the ALSA
// ioctl ABI, so unmodified libasound (aplay, amixer, PulseAudio, PipeWire)
// works. The core owns all of the ALSA state machine (hw_params refinement,
// ring-buffer pointers, xruns, draining, poll wakeups); a driver only has to
// program its DMA engine and report the hardware position, very much like
// a Linux snd_pcm_ops driver.
namespace genfs { class TextBuffer; }

namespace sound {

// errno values ALSA uses that the rest of the kernel doesn't (yet).
namespace snderr {
constexpr long BADFD = 77, STRPIPE = 86, ALREADY = 114;
}

// Copy a NUL-terminated name into a fixed-size ABI field (always terminated, zero-padded).
inline void copy_name(void* dst, const char* src, usize size) {
    auto* d = static_cast<char*>(dst);
    usize i = 0;
    for (; src && src[i] && i + 1 < size; i++)
        d[i] = src[i];
    for (; i < size; i++)
        d[i] = 0;
}

struct Card;
struct Pcm;
struct Substream;

// What a stream's hardware supports (Linux: struct snd_pcm_hardware).
struct PcmHardware {
    u32 info = abi::INFO_INTERLEAVED | abi::INFO_MMAP | abi::INFO_MMAP_VALID | abi::INFO_BLOCK_TRANSFER |
               abi::INFO_PAUSE;
    u64 formats = 0;               // bit (1 << abi::FORMAT_x) per supported sample format
    u32 rates[16] = {};            // supported sample rates, ascending
    u32 rate_count = 0;
    u32 channels_min = 1, channels_max = 2;
    u32 buffer_bytes_max = 0;      // 0 = as large as the core's DMA buffer
    u32 period_bytes_min = 128, period_bytes_max = 0; // 0 = buffer_bytes_max
    u32 periods_min = 2, periods_max = 32;
    u32 byte_align = 0;            // period/buffer byte sizes must be multiples of this (0 = none)
    u32 msbits = 0;                // significant bits of 32-bit samples (24 for most HDA codecs), 0 = all
};

// The configuration negotiated by HW_PARAMS (read-only for drivers).
struct PcmConfig {
    int access = 0;
    int format = 0;
    u32 channels = 0;
    u32 rate = 0;
    u32 sample_bits = 0;   // physical bits per sample
    u32 frame_bytes = 0;
    u64 period_size = 0;   // frames
    u64 periods = 0;
    u64 buffer_size = 0;   // frames (period_size * periods)
};

enum TriggerCmd : int { TRIGGER_STOP = 0, TRIGGER_START = 1, TRIGGER_PAUSE_PUSH = 3, TRIGGER_PAUSE_RELEASE = 4 };

// Driver callbacks for one stream.
struct PcmOps {
    // Program the stream for s->cfg (format, rate, channels, the DMA buffer
    // split into s->cfg.periods periods) and reset its position to 0. May sleep.
    long (*prepare)(Substream* s);
    // Start/stop/pause the DMA engine. Runs with the stream lock held and
    // interrupts off: must not sleep.
    long (*trigger)(Substream* s, int cmd);
    // Current DMA position in frames, 0 .. buffer_size-1 (stream lock held).
    u64 (*pointer)(Substream* s);
    // Optional: the stream was closed or reconfigured; release what prepare set up. May sleep.
    void (*hw_free)(Substream* s);
};

// One direction of one PCM device (Linux: snd_pcm_substream + runtime).
struct Substream {
    Card* card = nullptr;
    Pcm* pcm = nullptr;
    int stream = 0;            // abi::STREAM_PLAYBACK / STREAM_CAPTURE
    PcmHardware hw;
    const PcmOps* ops = nullptr;
    void* driver_data = nullptr;

    // The DMA ring buffer. Allocated once at registration (physically
    // contiguous) and never freed, so user mappings of it can't dangle.
    u64 dma_phys = 0;
    u8* dma_area = nullptr;
    u32 dma_bytes = 0;

    PcmConfig cfg;

    // ---- Internal state (the core's; drivers leave it alone) ----
    // The status and control records live in their own pages so that
    // libasound can mmap them, exactly like Linux.
    abi::MmapStatus* status = nullptr;
    abi::MmapControl* control = nullptr;
    u64 status_phys = 0, control_phys = 0;
    bool open = false;
    bool configured = false;
    u64 boundary = 0;
    u64 start_threshold = 0, stop_threshold = 0, silence_threshold = 0, silence_size = 0;
    int tstamp_mode = 0, tstamp_type = 0;
    u64 hw_ptr_base = 0;       // hw_ptr of the start of the current buffer lap
    u64 last_pos = 0;          // driver position at the previous update
    u64 last_update_ns = 0;
    u64 avail_max = 0;
    u64 xruns = 0;
    abi::Timespec trigger_tstamp = {};
    sched::Mutex config_lock;  // serializes the configuration ioctls (may sleep)
    sched::WaitQueue wait;     // its lock protects status/control/state; writers/readers sleep here
    void* timers = nullptr;    // PCM timer instances bound to this stream (timer.cpp)
    u32 open_count = 0;        // opens sharing the stream (O_APPEND: dmix's secondary clients)
};

struct Pcm {
    Card* card = nullptr;
    u32 device = 0;
    char id[64] = {};
    char name[80] = {};
    Substream* streams[2] = {}; // [playback, capture], null if absent
};

// A mixer control element (Linux: snd_kcontrol), e.g. "Master Playback Volume".
struct Control {
    char name[44] = {};
    i32 iface = abi::IFACE_MIXER;
    u32 device = 0, index = 0;
    i32 type = abi::ELEM_TYPE_INTEGER; // BOOLEAN or INTEGER
    u32 count = 1;                     // values (channels)
    i64 min = 0, max = 1, step = 1;
    // dB scale for volume controls (TLV): value v means db_min + v*db_step (in 0.01 dB).
    bool has_db = false;
    i32 db_min = 0, db_step = 0;
    bool db_mute_at_min = false;
    long (*get)(Control* c, i64* values) = nullptr;
    // Returns 1 if the value changed, 0 if not, or a negative errno.
    long (*put)(Control* c, const i64* values) = nullptr;
    void* driver_data = nullptr;

    // Filled in by the core.
    Card* card = nullptr;
    u32 numid = 0;
};

constexpr u32 MAX_PCMS = 4;
constexpr u32 MAX_CONTROLS = 64;

struct Card {
    int number = -1;
    char id[16] = {};        // short identifier ("Intel", "PCH"), used by "hw:CARD=<id>"
    char driver[16] = {};    // driver name; libasound loads /usr/share/alsa/cards/<driver>.conf
    char name[32] = {};
    char longname[80] = {};
    char mixername[80] = {};
    char components[128] = {};
    dev::Device* device = nullptr;
    Pcm* pcms[MAX_PCMS] = {};
    u32 pcm_count = 0;
    Control* controls[MAX_CONTROLS] = {};
    u32 control_count = 0;
    sched::Mutex control_lock;  // serializes control get/put (drivers may sleep in them)
    void* events = nullptr;     // control event subscribers (core-internal)
    bool registered = false;
};

// ---- Driver API ----
void init(); // /dev/snd, /proc/asound (before any driver probes)

Card* card_create(const char* id, const char* driver, const char* name, const char* longname, dev::Device* d);
// Add PCM device #card->pcm_count with the given streams.
Pcm* pcm_create(Card* card, const char* name, bool playback, bool capture);
// Describe a stream; allocates its DMA buffer. Returns false when out of memory.
bool pcm_set_ops(Pcm* pcm, int stream, const PcmHardware& hw, const PcmOps* ops, void* driver_data);
Control* control_add(Card* card, const Control& tmpl);
// Publish the card: device nodes, /proc/asound, /sys/class/sound.
long card_register(Card* card);

// A period boundary passed (call from the driver's interrupt worker; not
// from hard-IRQ context). Updates the pointers and wakes writers/pollers.
void period_elapsed(Substream* s);
// A control's value changed behind userspace's back (e.g. jack sensing).
void control_notify(Control* c);

// Add a generated text file /proc/asound/card<n>/<name> (e.g. a codec dump).
void card_proc_file(Card* card, const char* name, bool (*gen)(genfs::TextBuffer& out, const void* ctx), const void* ctx);

Card* card_by_number(int n);
u32 card_count();

// Sample-format helpers.
int format_physical_width(int format); // bits per sample in memory, -1 if unknown
int format_width(int format);          // significant bits
const char* format_name(int format);

// ---- Core internals shared between the core's files ----
namespace internal {
void register_pcm_node(Substream* s, const char* path);
void register_control_node(Card* card, const char* path);
void register_timer_node();
long refine(const Substream* s, abi::HwParams* params);   // HW_REFINE
long choose(const Substream* s, abi::HwParams* params);   // HW_PARAMS: refine + pick single values
void timer_kick(); // a stream started: make the position-update thread poll
bool update_running(Substream* s); // poll one stream's position; true if it is running
void timer_ticks(Substream* s, u64 periods); // PCM timers: the stream passed period boundaries
} // namespace internal

} // namespace sound
