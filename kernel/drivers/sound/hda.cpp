// Intel High Definition Audio: the controller (Intel ICH/PCH, AMD, NVIDIA,
// QEMU's intel-hda / ich9-intel-hda) and a generic codec driver.
//
// Controller side: the CORB/RIRB ring pair carries 32-bit commands ("verbs")
// to the codecs and their responses back; stream descriptors DMA audio
// between memory and the HDA link through a buffer descriptor list (BDL),
// one BDL entry per ALSA period with interrupt-on-completion.
//
// Codec side: a codec is a graph of widgets (converters, mixers, selectors,
// pin complexes) described by parameters and connection lists. Like Linux's
// generic parser we walk that graph: from every output jack (line out,
// headphone, speaker) find a path back to a DAC, from the microphone /
// line-in jack a path to an ADC, then set connection selects, amplifier
// gains/mutes, pin controls and EAPD along the way. That covers QEMU's
// codecs and the common Realtek ALC2xx/8xx layouts without per-codec tables.
//
// Mixer: "Master Playback Volume/Switch" and "PCM Playback Volume" act on the
// output paths' amplifiers (Master is a "virtual master" offsetting PCM, as
// Linux does), "Capture Volume/Switch" on the input path.

#include <arch/x86_64/interrupts.hpp>
#include <core/log.hpp>
#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/sound/sound_drivers.hpp>
#include <fs/genfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <sound/sound.hpp>
#include <time/clock.hpp>

namespace hda {

namespace {

// ---- Controller registers ----
constexpr u32 GCAP = 0x00, VMIN = 0x02, VMAJ = 0x03, GCTL = 0x08, STATESTS = 0x0E,
              INTCTL = 0x20, INTSTS = 0x24,
              CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4A, CORBCTL = 0x4C,
              CORBSIZE = 0x4E,
              RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5A, RIRBCTL = 0x5C, RIRBSTS = 0x5D,
              RIRBSIZE = 0x5E,
              ICOI = 0x60, ICII = 0x64, ICIS = 0x68,
              SD_BASE = 0x80, SD_STRIDE = 0x20;
// Stream descriptor registers (offset from its base).
constexpr u32 SD_CTL = 0x00, SD_STS = 0x03, SD_LPIB = 0x04, SD_CBL = 0x08, SD_LVI = 0x0C, SD_FMT = 0x12,
              SD_BDPL = 0x18, SD_BDPU = 0x1C;
constexpr u32 SD_CTL_SRST = 1 << 0, SD_CTL_RUN = 1 << 1, SD_CTL_IOCE = 1 << 2, SD_CTL_FEIE = 1 << 3,
              SD_CTL_DEIE = 1 << 4;
constexpr u8 SD_STS_MASK = 0x1C; // BCIS | FIFOE | DESE (write 1 to clear)
constexpr u32 INTCTL_GIE = 1u << 31, INTCTL_CIE = 1u << 30;

// ---- Codec verbs and parameters ----
constexpr u32 VERB_GET_PARAM = 0xF00, VERB_GET_CONN_LIST = 0xF02,
              VERB_SET_CONN_SELECT = 0x701, VERB_SET_POWER = 0x705, VERB_SET_STREAM = 0x706,
              VERB_SET_PIN_CTL = 0x707, VERB_SET_EAPD = 0x70C,
              VERB_GET_CONFIG = 0xF1C, VERB_SET_CHANNEL_COUNT = 0x72D;
// 4-bit verbs with a 16-bit payload.
constexpr u32 VERB_SET_FORMAT = 0x2, VERB_SET_AMP = 0x3;

constexpr u32 PARAM_VENDOR = 0x00, PARAM_NODE_COUNT = 0x04, PARAM_FG_TYPE = 0x05, PARAM_WIDGET_CAPS = 0x09,
              PARAM_PCM = 0x0A, PARAM_STREAM_FORMATS = 0x0B, PARAM_PIN_CAPS = 0x0C, PARAM_AMP_IN = 0x0D,
              PARAM_CONN_LEN = 0x0E, PARAM_AMP_OUT = 0x12;

enum WidgetType : u8 { W_DAC = 0, W_ADC = 1, W_MIXER = 2, W_SELECTOR = 3, W_PIN = 4, W_POWER = 5, W_KNOB = 6,
                       W_BEEP = 7, W_VENDOR = 15 };
constexpr u32 WCAP_STEREO = 1 << 0, WCAP_IN_AMP = 1 << 1, WCAP_OUT_AMP = 1 << 2, WCAP_AMP_OVERRIDE = 1 << 3,
              WCAP_FORMAT_OVERRIDE = 1 << 4, WCAP_CONN_LIST = 1 << 8, WCAP_DIGITAL = 1 << 9;
constexpr u32 PINCAP_HP = 1 << 3, PINCAP_OUT = 1 << 4, PINCAP_IN = 1 << 5,
              PINCAP_EAPD = 1 << 16;
constexpr u32 PINCAP_VREF_80 = 1 << (8 + 4);
constexpr u8 PINCTL_OUT = 0x40, PINCTL_IN = 0x20, PINCTL_HP = 0x80, PINCTL_VREF_80 = 0x04;

// Default configuration: device types and connectivity.
enum : u8 { DEV_LINE_OUT = 0, DEV_SPEAKER = 1, DEV_HP_OUT = 2, DEV_LINE_IN = 8, DEV_AUX = 9, DEV_MIC = 0xA };
constexpr u8 CONN_NONE = 1;

constexpr u32 MAX_NID = 128;
constexpr u32 MAX_CONN = 32;
constexpr u32 MAX_PATH = 8;
constexpr u32 MAX_OUT_PATHS = 4;

struct Widget {
    bool present = false;
    u8 type = 0;
    u32 caps = 0, pincaps = 0, config = 0;
    u32 amp_in = 0, amp_out = 0; // amplifier capabilities (after the AFG default)
    u32 pcm = 0, formats = 0;    // converters: supported rates/sizes and stream formats
    u8 conn[MAX_CONN] = {};
    u8 nconn = 0;
};

// A route through the widget graph. nodes[0] is the end the user sees
// (output: the pin, input: the ADC); idx[i] is the entry of nodes[i]'s
// connection list that leads to nodes[i+1].
struct Path {
    u8 nodes[MAX_PATH] = {};
    u8 idx[MAX_PATH] = {};
    u8 len = 0;
};

// An amplifier on a path: widget, direction and input index.
struct Amp {
    u8 nid = 0;
    bool output = false;
    u8 index = 0;
    u32 caps = 0;
    u8 steps() const { return (caps >> 8) & 0x7F; }
    u8 offset() const { return caps & 0x7F; }
    u32 step_cdb() const { return (((caps >> 16) & 0x7F) + 1) * 25; } // 0.25 dB units -> 0.01 dB
    bool can_mute() const { return caps & (1u << 31); }
};

constexpr u32 MAX_AMPS = 12;

struct AmpGroup {
    Amp volume[MAX_OUT_PATHS]; // gain amps (one per path)
    u32 nvolume = 0;
    Amp mute[MAX_AMPS];        // every mute-capable amp on the paths
    u32 nmute = 0;
};

struct BdlEntry {
    u64 address;
    u32 length;
    u32 flags; // bit 0: interrupt on completion
};

class Controller;

struct Stream {
    Controller* hc = nullptr;
    u32 sd = 0;            // stream descriptor index
    u8 tag = 0;            // stream number on the link (1-15)
    bool capture = false;
    sound::Substream* sub = nullptr;
    u64 bdl_phys = 0;
    BdlEntry* bdl = nullptr;
    u8 converters[MAX_OUT_PATHS] = {}; // DACs (or the ADC) fed by this stream
    u32 nconverters = 0;
    u16 format = 0;
    volatile bool running = false;
};

class Controller {
public:
    volatile u8* regs = nullptr;
    pci::Function* pci = nullptr;
    dev::Device* hw = nullptr;
    u32 iss = 0, oss = 0;

    // CORB/RIRB
    u32* corb = nullptr;
    volatile u64* rirb = nullptr;
    u32 corb_entries = 0, rirb_entries = 0;
    u16 rirb_rp = 0;
    bool use_immediate = false;
    sched::Mutex cmd_lock;

    // The codec we drive.
    u8 codec = 0;
    u32 vendor_id = 0;
    u32 subsystem_id = 0;
    u8 afg = 0;
    Widget w[MAX_NID];
    u8 nid_start = 0, nid_count = 0;

    Path out_paths[MAX_OUT_PATHS];
    u32 nout = 0;
    Path in_path;
    bool has_input = false;

    Stream play, cap;
    sched::Semaphore irq_signal;
    sound::Card* card = nullptr;

    // Mixer state: [left, right]
    AmpGroup out_amps, in_amps;
    i64 master[2] = {}, pcm[2] = {}, capture[2] = {};
    bool master_on[2] = {true, true}, capture_on[2] = {true, true};
    u8 vol_steps = 0;     // range of Master/PCM (the first output path's gain amp)
    u8 cap_steps = 0;

    template <typename T> T rd(u32 off) { return *reinterpret_cast<volatile T*>(regs + off); }
    template <typename T> void wr(u32 off, T v) { *reinterpret_cast<volatile T*>(regs + off) = v; }
    u32 sd_base(u32 sd) const { return SD_BASE + sd * SD_STRIDE; }

    bool reset();
    bool setup_rings();
    bool command(u32 cmd, u32* response);
    u32 verb(u8 nid, u32 verb, u32 payload);
    u32 param(u8 nid, u32 id) { return verb(nid, VERB_GET_PARAM, id); }
    bool probe_codec(u8 addr);
    void apply_quirks();
    void start_jack_sense();
    void silence_unused();
    void log_codec();
    bool apple_gpio_amp = false;
    u8 gpio_speaker = 0x01;   // GPIO bits that power the speaker amplifier
    u8 gpio_headphone = 0x00; // ... and the headphone amplifier (0: none)
    void read_widget(u8 nid);
    bool find_path(u8 from, bool to_dac, const u8* preferred, u32 npreferred, Path* out);
    void enable_output_path(const Path& p, bool headphone);
    void enable_input_path(const Path& p);
    void collect_amps(const Path& p, bool input, AmpGroup* g);
    void set_amp(const Amp& a, int channel, u8 gain, bool mute);
    void apply_output_volume();
    void apply_capture_volume();
    void setup_mixer();
    bool setup_stream(Stream& st, u32 sd, u8 tag, bool capture);
    sound::PcmHardware hardware_for(u8 converter, bool capture);
};

// ---- Command transport ----

bool wait_for(auto cond, u64 timeout_ns) {
    u64 deadline = clock::now_ns() + timeout_ns;
    while (!cond()) {
        if (clock::now_ns() > deadline)
            return cond();
        arch::pause();
    }
    return true;
}

bool Controller::reset() {
    // Stop the rings and every stream before resetting the link.
    wr<u8>(CORBCTL, 0);
    wr<u8>(RIRBCTL, 0);
    for (u32 sd = 0; sd < iss + oss; sd++)
        wr<u32>(sd_base(sd) + SD_CTL, rd<u32>(sd_base(sd) + SD_CTL) & ~(SD_CTL_RUN | SD_CTL_IOCE));
    wr<u32>(INTCTL, 0);
    wr<u16>(STATESTS, 0xFFFF);

    wr<u32>(GCTL, rd<u32>(GCTL) & ~1u); // enter reset
    if (!wait_for([&] { return (rd<u32>(GCTL) & 1) == 0; }, 100'000'000))
        return false;
    sched::sleep_ms(1);
    wr<u32>(GCTL, rd<u32>(GCTL) | 1u); // leave reset
    if (!wait_for([&] { return (rd<u32>(GCTL) & 1) == 1; }, 100'000'000))
        return false;
    // Codecs need up to 521 us after reset to request a status change; Linux waits longer.
    sched::sleep_ms(10);
    wr<u32>(GCTL, rd<u32>(GCTL) | (1u << 8)); // accept unsolicited responses
    return true;
}

bool Controller::setup_rings() {
    u64 phys = pmm::alloc_page(true); // CORB (1 KiB) + RIRB (2 KiB) share one page
    if (!phys)
        return false;
    corb = mm::phys_to_virt<u32>(phys);
    rirb = mm::phys_to_virt<u64>(phys + 2048);

    // Largest supported ring sizes (capability bits 4-6: 2, 16, 256 entries).
    auto pick = [](u8 cap, u32* entries) -> u8 {
        if (cap & 0x40) { *entries = 256; return 2; }
        if (cap & 0x20) { *entries = 16; return 1; }
        *entries = 2;
        return 0;
    };
    u8 corb_size = pick(rd<u8>(CORBSIZE), &corb_entries);
    u8 rirb_size = pick(rd<u8>(RIRBSIZE), &rirb_entries);

    wr<u8>(CORBCTL, 0);
    wait_for([&] { return (rd<u8>(CORBCTL) & 2) == 0; }, 10'000'000);
    wr<u8>(CORBSIZE, (rd<u8>(CORBSIZE) & ~3) | corb_size);
    wr<u32>(CORBLBASE, static_cast<u32>(phys));
    wr<u32>(CORBUBASE, static_cast<u32>(phys >> 32));
    wr<u16>(CORBWP, 0);
    // Reset the read pointer: set bit 15, wait for it to read back, clear it.
    // Some controllers (and QEMU) don't echo the bit; don't insist.
    wr<u16>(CORBRP, 0x8000);
    wait_for([&] { return (rd<u16>(CORBRP) & 0x8000) != 0; }, 1'000'000);
    wr<u16>(CORBRP, 0);
    wait_for([&] { return (rd<u16>(CORBRP) & 0x8000) == 0; }, 1'000'000);
    wr<u8>(CORBCTL, 2); // DMA run

    wr<u8>(RIRBCTL, 0);
    wait_for([&] { return (rd<u8>(RIRBCTL) & 2) == 0; }, 10'000'000);
    wr<u8>(RIRBSIZE, (rd<u8>(RIRBSIZE) & ~3) | rirb_size);
    wr<u32>(RIRBLBASE, static_cast<u32>(phys + 2048));
    wr<u32>(RIRBUBASE, static_cast<u32>((phys + 2048) >> 32));
    wr<u16>(RIRBWP, 0x8000); // reset the write pointer
    wr<u16>(RINTCNT, 1);
    // DMA run + response "interrupt" status. We poll and never enable CIE in
    // INTCTL, but the status bit must be raised and cleared: controllers
    // (QEMU's, and per spec) stop processing the CORB after RINTCNT
    // responses until software acknowledges RIRBSTS.RINTFL.
    wr<u8>(RIRBCTL, 3);
    rirb_rp = 0;
    return true;
}

// Send one verb and wait for its response (polling; verbs are rare).
bool Controller::command(u32 cmd, u32* response) {
    if (use_immediate) {
        // Immediate Command interface: one command at a time through registers.
        if (!wait_for([&] { return (rd<u16>(ICIS) & 1) == 0; }, 10'000'000))
            return false;
        wr<u16>(ICIS, 2); // clear "result valid"
        wr<u32>(ICOI, cmd);
        wr<u16>(ICIS, 1); // busy: send it
        if (!wait_for([&] { return (rd<u16>(ICIS) & 3) == 2; }, 10'000'000))
            return false;
        *response = rd<u32>(ICII);
        return true;
    }
    u16 wp = static_cast<u16>((rd<u16>(CORBWP) + 1) % corb_entries);
    corb[wp] = cmd;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr<u16>(CORBWP, wp);
    u64 deadline = clock::now_ns() + 50'000'000;
    for (;;) {
        u16 rirb_wp = rd<u16>(RIRBWP) & 0xFF;
        while (rirb_rp != rirb_wp) {
            rirb_rp = static_cast<u16>((rirb_rp + 1) % rirb_entries);
            u64 entry = rirb[rirb_rp];
            u32 ex = static_cast<u32>(entry >> 32);
            if (ex & (1 << 4))
                continue; // unsolicited response (jack events): not handled yet
            *response = static_cast<u32>(entry);
            wr<u8>(RIRBSTS, 0x5);
            return true;
        }
        if (clock::now_ns() > deadline)
            return false;
        arch::pause();
    }
}

u32 Controller::verb(u8 nid, u32 v, u32 payload) {
    u32 cmd = (static_cast<u32>(codec) << 28) | (static_cast<u32>(nid) << 20);
    cmd |= v >= 0x100 ? (v << 8) | (payload & 0xFF) : (v << 16) | (payload & 0xFFFF);
    LockGuard guard(cmd_lock);
    u32 resp = 0;
    if (!command(cmd, &resp)) {
        if (!use_immediate) { // the CORB/RIRB path seems broken: fall back to immediate commands
            klog::warn("hda: no response over CORB/RIRB (CORBWP %u CORBRP %u RIRBWP %u RIRBSTS %x CORBCTL %x RIRBCTL %x), using immediate commands",
                       rd<u16>(CORBWP), rd<u16>(CORBRP), rd<u16>(RIRBWP), rd<u8>(RIRBSTS), rd<u8>(CORBCTL), rd<u8>(RIRBCTL));
            use_immediate = true;
            if (command(cmd, &resp))
                return resp;
        }
        return 0;
    }
    return resp;
}

// ---- Codec discovery ----

void Controller::read_widget(u8 nid) {
    Widget& x = w[nid];
    x.present = true;
    x.caps = param(nid, PARAM_WIDGET_CAPS);
    x.type = (x.caps >> 20) & 0xF;
    u32 afg_in = param(afg, PARAM_AMP_IN), afg_out = param(afg, PARAM_AMP_OUT);
    if (x.caps & WCAP_IN_AMP)
        x.amp_in = (x.caps & WCAP_AMP_OVERRIDE) ? param(nid, PARAM_AMP_IN) : afg_in;
    if (x.caps & WCAP_OUT_AMP)
        x.amp_out = (x.caps & WCAP_AMP_OVERRIDE) ? param(nid, PARAM_AMP_OUT) : afg_out;
    if (x.type == W_DAC || x.type == W_ADC) {
        bool own = x.caps & WCAP_FORMAT_OVERRIDE;
        x.pcm = param(own ? nid : afg, PARAM_PCM);
        x.formats = param(own ? nid : afg, PARAM_STREAM_FORMATS);
    }
    if (x.type == W_PIN) {
        x.pincaps = param(nid, PARAM_PIN_CAPS);
        x.config = verb(nid, VERB_GET_CONFIG, 0);
    }
    if (x.caps & WCAP_CONN_LIST) {
        u32 len = param(nid, PARAM_CONN_LEN);
        bool long_form = len & 0x80;
        u32 count = len & 0x7F;
        u32 per = long_form ? 2 : 4;
        u32 bits = long_form ? 16 : 8;
        u32 prev = 0;
        for (u32 i = 0; i < count && x.nconn < MAX_CONN; i += per) {
            u32 entries = verb(nid, VERB_GET_CONN_LIST, i);
            for (u32 k = 0; k < per && i + k < count && x.nconn < MAX_CONN; k++) {
                u32 e = (entries >> (k * bits)) & ((1u << bits) - 1);
                bool range = e & (1u << (bits - 1));
                u32 id = e & ((1u << (bits - 1)) - 1);
                if (range && prev) { // "prev..id" shorthand
                    for (u32 n = prev + 1; n <= id && x.nconn < MAX_CONN; n++)
                        x.conn[x.nconn++] = static_cast<u8>(n);
                } else {
                    x.conn[x.nconn++] = static_cast<u8>(id);
                }
                prev = id;
            }
        }
    }
}

bool Controller::probe_codec(u8 addr) {
    codec = addr;
    vendor_id = param(0, PARAM_VENDOR);
    if (!vendor_id || vendor_id == 0xFFFFFFFF)
        return false;
    u32 nodes = param(0, PARAM_NODE_COUNT);
    u8 fg_start = (nodes >> 16) & 0xFF, fg_count = nodes & 0xFF;
    afg = 0;
    for (u8 n = fg_start; n < fg_start + fg_count; n++)
        if ((param(n, PARAM_FG_TYPE) & 0xFF) == 1) // audio function group
            afg = n;
    if (!afg)
        return false; // a modem codec
    verb(afg, VERB_SET_POWER, 0); // D0
    sched::sleep_ms(10);
    nodes = param(afg, PARAM_NODE_COUNT);
    nid_start = (nodes >> 16) & 0xFF;
    nid_count = nodes & 0xFF;
    for (auto& x : w)
        x = Widget{};
    for (u32 n = nid_start; n < nid_start + nid_count && n < MAX_NID; n++)
        read_widget(static_cast<u8>(n));
    apply_quirks();
    return true;
}

// Codec/machine specific fixes the generic parser can't discover (codec
// facts as documented by Linux's HDA codec drivers).
void Controller::apply_quirks() {
    u32 ssid = verb(afg, 0xF20, 0); // subsystem ID: board vendor << 16 | model
    subsystem_id = ssid;
    if (vendor_id == 0x10134208 && (ssid >> 16) == 0x106B) {
        // Cirrus Logic CS4208 in Macs (MacBook Air 2013+, MacBook Pro 11/12,
        // Mac mini 7, iMac 16): vendor processing block on...
        verb(0x24, 0x703, 0x01);   // processing state: on
        verb(0x24, 0x5, 0x0033);   // coefficient index
        verb(0x24, 0x4, 0x0001);   // A1 ICS
        verb(0x24, 0x5, 0x0034);
        verb(0x24, 0x4, 0x1C01);   // A1 enable, threshold 300 mV
        u16 model = ssid & 0xFFFF;
        if (model == 0x7100 || model == 0x7200) {
            // MacBook Air 6,1 / 6,2: the firmware's pin table is wrong.
            static const struct { u8 nid; u32 config; } pins[] = {
                {0x10, 0x032120F0}, // headphones
                {0x11, 0x500000F0}, {0x12, 0x90100010}, // speaker
                {0x13, 0x500000F0}, {0x14, 0x500000F0}, {0x15, 0x770000F0}, {0x16, 0x770000F0},
                {0x17, 0x430000F0}, {0x18, 0x43AB9030}, // mic jack
                {0x19, 0x770000F0}, {0x1A, 0x770000F0}, {0x1B, 0x770000F0},
                {0x1C, 0x90A00090}, // internal mic
                {0x1D, 0x500000F0}, {0x1E, 0x500000F0}, {0x1F, 0x500000F0}, {0x20, 0x500000F0},
                {0x21, 0x430000F0}, {0x22, 0x430000F0},
            };
            for (const auto& pc : pins)
                if (pc.nid < MAX_NID && w[pc.nid].type == W_PIN)
                    w[pc.nid].config = pc.config;
        }
        // The speaker amplifier hangs off GPIO0: on (off while headphones are in).
        apple_gpio_amp = true;
        verb(afg, 0x716, 0x01); // GPIO mask
        verb(afg, 0x717, 0x01); // GPIO direction: output
        verb(afg, 0x715, 0x01); // GPIO data: amp on
        klog::info("hda: Apple CS4208 (board %04x): processing block, %sspeaker amplifier (GPIO0) on", model,
                   (model == 0x7100 || model == 0x7200) ? "MacBook Air pin table, " : "");
    }
    if ((vendor_id == 0x10134206 || vendor_id == 0x10134207) && (ssid >> 16) == 0x106B) {
        // Cirrus Logic CS4206/CS4207 in Macs (iMac 12/13, MacBook Pro 8-10,
        // MacBook Air 4/5): amplifiers on GPIO1 (headphones) and GPIO3
        // (speakers), as Linux's patch_cirrus "apple" fixup. Off = silence.
        apple_gpio_amp = true;
        gpio_speaker = 0x08;
        gpio_headphone = 0x02;
        verb(afg, 0x716, 0x0A); // GPIO mask
        verb(afg, 0x717, 0x0A); // GPIO direction: outputs
        verb(afg, 0x715, gpio_speaker); // speakers on until headphones are seen
        klog::info("hda: Apple CS%04x (board %04x): speaker amplifier (GPIO3) on, headphone amplifier on GPIO1",
                   vendor_id & 0xFFFF, ssid & 0xFFFF);
    }
}

u8 pin_device(const Widget& x);
u8 pin_connectivity(const Widget& x);

// ---- headphone jack sensing ----
// Plugging headphones in mutes the speakers (and their amplifier), like
// every laptop does. Pins are polled (pin sense verb) every 300 ms.
namespace {

struct JackState {
    Controller* hc;
    u8 hp[4], nhp = 0;
    u8 spk[4], nspk = 0;
    bool last = false;
};

constexpr u32 PINCAP_TRIGGER = 1u << 1, PINCAP_PRESENCE = 1u << 2;

bool pin_present(Controller* hc, u8 pin) {
    if (hc->w[pin].pincaps & PINCAP_TRIGGER) {
        hc->verb(pin, 0x709, 0); // execute pin sense
        sched::sleep_ms(1);
    }
    return hc->verb(pin, 0xF09, 0) & 0x80000000u;
}

void jack_thread(void* arg) {
    auto* js = static_cast<JackState*>(arg);
    Controller* hc = js->hc;
    for (;;) {
        bool any = false;
        for (u8 i = 0; i < js->nhp; i++)
            any |= pin_present(hc, js->hp[i]);
        if (any != js->last) {
            js->last = any;
            for (u8 i = 0; i < js->nspk; i++)
                hc->verb(js->spk[i], 0x707 /* pin control */, any ? 0 : 0x40 /* out */);
            if (hc->apple_gpio_amp)
                hc->verb(hc->afg, 0x715, any ? hc->gpio_headphone : hc->gpio_speaker);
            klog::info("hda: headphones %s: speakers %s", any ? "plugged in" : "unplugged", any ? "off" : "on");
        }
        sched::sleep_ms(300);
    }
}

} // namespace

// Nothing we don't route may make sound: mixers off our paths get every
// input muted (analog loopbacks feed microphones to the speakers), unused
// pins are switched off (no output, no mic bias).
void Controller::silence_unused() {
    bool used[MAX_NID] = {};
    for (u32 i = 0; i < nout; i++)
        for (u8 k = 0; k < out_paths[i].len; k++)
            used[out_paths[i].nodes[k]] = true;
    if (has_input)
        for (u8 k = 0; k < in_path.len; k++)
            used[in_path.nodes[k]] = true;
    for (u32 n = 0; n < MAX_NID; n++) {
        Widget& x = w[n];
        if (!x.present || used[n])
            continue;
        if (x.type == W_MIXER && (x.caps & WCAP_IN_AMP))
            for (u8 k = 0; k < x.nconn; k++) {
                Amp a{static_cast<u8>(n), false, k, x.amp_in};
                set_amp(a, -1, 0, true);
            }
        if (x.type == W_PIN && !(x.caps & WCAP_DIGITAL))
            verb(static_cast<u8>(n), VERB_SET_PIN_CTL, 0);
    }
}

// The codec's widget graph in the kernel log (real-hardware diagnostics).
void Controller::log_codec() {
    klog::info("hda: codec %08x subsystem %08x widgets:", vendor_id, subsystem_id);
    static const char* const types[] = {"DAC", "ADC", "MIX", "SEL", "PIN", "PWR", "VOL", "BEEP"};
    for (u32 n = 0; n < MAX_NID; n++) {
        const Widget& x = w[n];
        if (!x.present)
            continue;
        char conn[64] = {};
        u32 len = 0;
        for (u8 k = 0; k < x.nconn && len + 4 < sizeof(conn); k++)
            len += snprintf(conn + len, sizeof(conn) - len, "%02x ", x.conn[k]);
        klog::info("hda:  %02x %s caps %08x pin %08x cfg %08x ampin %08x ampout %08x conn %s", n,
                   x.type < 8 ? types[x.type] : "?", x.caps, x.pincaps, x.config, x.amp_in, x.amp_out, conn);
    }
    for (u32 i = 0; i < nout; i++) {
        char p[48] = {};
        u32 len = 0;
        for (u8 k = 0; k < out_paths[i].len && len + 4 < sizeof(p); k++)
            len += snprintf(p + len, sizeof(p) - len, "%02x ", out_paths[i].nodes[k]);
        klog::info("hda:  output path %u: %s", i, p);
    }
    if (has_input) {
        char p[48] = {};
        u32 len = 0;
        for (u8 k = 0; k < in_path.len && len + 4 < sizeof(p); k++)
            len += snprintf(p + len, sizeof(p) - len, "%02x ", in_path.nodes[k]);
        klog::info("hda:  input path: %s", p);
    }
}

void Controller::start_jack_sense() {
    auto* js = new JackState;
    js->hc = this;
    for (u32 i = 0; i < nout; i++) {
        u8 pin = out_paths[i].nodes[0];
        const Widget& x = w[pin];
        u8 dev = pin_device(x);
        bool jack = pin_connectivity(x) == 0;          // an external jack
        bool detect = !(x.config & (1u << 8));         // "jack detect override" not set
        if (dev == DEV_HP_OUT && jack && detect && (x.pincaps & PINCAP_PRESENCE) && js->nhp < 4)
            js->hp[js->nhp++] = pin;
        else if (dev == DEV_SPEAKER && js->nspk < 4)
            js->spk[js->nspk++] = pin;
    }
    if (!js->nhp || !js->nspk) {
        delete js; // nothing to switch
        return;
    }
    sched::spawn("hda-jack", jack_thread, js, -1, true);
}

u8 pin_device(const Widget& x) { return (x.config >> 20) & 0xF; }
u8 pin_connectivity(const Widget& x) { return (x.config >> 30) & 3; }

// Breadth-first search from `from` through connection lists to a DAC
// (to_dac) or to an input pin. Prefers targets listed in `preferred`.
bool Controller::find_path(u8 from, bool to_dac, const u8* preferred, u32 npreferred, Path* out) {
    for (int pass = npreferred ? 0 : 1; pass < 2; pass++) {
        u8 parent[MAX_NID], parent_idx[MAX_NID], depth[MAX_NID];
        bool seen[MAX_NID] = {};
        u8 queue[MAX_NID];
        u32 head = 0, tail = 0;
        queue[tail++] = from;
        seen[from] = true;
        depth[from] = 0;
        while (head < tail) {
            u8 n = queue[head++];
            const Widget& x = w[n];
            bool target;
            if (to_dac)
                target = n != from && x.type == W_DAC && !(x.caps & WCAP_DIGITAL);
            else
                target = n != from && x.type == W_PIN && (x.pincaps & PINCAP_IN) && pin_connectivity(x) != CONN_NONE &&
                         !(x.caps & WCAP_DIGITAL);
            if (target && pass == 0) {
                bool pref = false;
                for (u32 i = 0; i < npreferred; i++)
                    pref |= preferred[i] == n;
                target = pref;
            }
            if (target && !to_dac) {
                // Input pins: only mic / line-in / aux jacks, not output-only ones that happen to be input capable.
                u8 d = pin_device(x);
                target = d == DEV_MIC || d == DEV_LINE_IN || d == DEV_AUX;
                if (target && pass == 0)
                    target = d == DEV_MIC; // first choice: a microphone
            }
            if (target) {
                Path p;
                u8 chain[MAX_PATH];
                u8 len = 0;
                for (u8 c = n;; c = parent[c]) {
                    chain[len++] = c;
                    if (c == from || len == MAX_PATH)
                        break;
                }
                // chain is target..from; reverse into from..target with connection indices.
                for (u8 i = 0; i < len; i++)
                    p.nodes[i] = chain[len - 1 - i];
                for (u8 i = 0; i + 1 < len; i++)
                    p.idx[i] = parent_idx[p.nodes[i + 1]];
                p.len = len;
                *out = p;
                return true;
            }
            if (n != from && (x.type == W_PIN || x.type == W_DAC || x.type == W_ADC))
                continue; // paths don't run through other endpoints
            if (depth[n] + 1 >= MAX_PATH)
                continue;
            for (u8 i = 0; i < x.nconn; i++) {
                u8 c = x.conn[i];
                if (c >= MAX_NID || !w[c].present || seen[c])
                    continue;
                seen[c] = true;
                parent[c] = n;
                parent_idx[c] = i;
                depth[c] = static_cast<u8>(depth[n] + 1);
                queue[tail++] = c;
            }
        }
        if (!to_dac && pass == 0)
            continue; // no mic: accept line-in/aux on the second pass
    }
    return false;
}

void Controller::set_amp(const Amp& a, int channel, u8 gain, bool mute) {
    u32 payload = (a.output ? 0x8000u : 0x4000u) | (static_cast<u32>(a.index & 0xF) << 8) | (mute ? 0x80 : 0) |
                  min<u8>(gain, a.steps());
    payload |= channel < 0 ? 0x3000 : channel == 0 ? 0x2000 : 0x1000;
    verb(a.nid, VERB_SET_AMP, payload);
}

void Controller::enable_output_path(const Path& p, bool headphone) {
    for (u8 i = 0; i < p.len; i++) {
        u8 n = p.nodes[i];
        Widget& x = w[n];
        verb(n, VERB_SET_POWER, 0);
        if (i + 1 < p.len) {
            if (x.type == W_MIXER) {
                // Unmute the input from the path, mute the others (analog loopback inputs hum).
                if (x.caps & WCAP_IN_AMP)
                    for (u8 k = 0; k < x.nconn; k++) {
                        Amp a{n, false, k, x.amp_in};
                        set_amp(a, -1, k == p.idx[i] ? a.offset() : 0, k != p.idx[i]);
                    }
            } else if (x.nconn > 1) {
                verb(n, VERB_SET_CONN_SELECT, p.idx[i]);
            }
        }
        if (x.caps & WCAP_OUT_AMP) {
            Amp a{n, true, 0, x.amp_out};
            set_amp(a, -1, a.offset(), false);
        }
    }
    u8 pin = p.nodes[0];
    u8 ctl = PINCTL_OUT;
    if (headphone && (w[pin].pincaps & PINCAP_HP))
        ctl |= PINCTL_HP;
    verb(pin, VERB_SET_PIN_CTL, ctl);
    if (w[pin].pincaps & PINCAP_EAPD)
        verb(pin, VERB_SET_EAPD, 0x02); // external amplifier on (laptop speakers)
}

void Controller::enable_input_path(const Path& p) {
    for (u8 i = 0; i < p.len; i++) {
        u8 n = p.nodes[i];
        Widget& x = w[n];
        verb(n, VERB_SET_POWER, 0);
        if (i + 1 < p.len) {
            if (x.type == W_MIXER) {
                if (x.caps & WCAP_IN_AMP)
                    for (u8 k = 0; k < x.nconn; k++) {
                        Amp a{n, false, k, x.amp_in};
                        set_amp(a, -1, a.offset(), k != p.idx[i]);
                    }
            } else {
                if (x.nconn > 1)
                    verb(n, VERB_SET_CONN_SELECT, p.idx[i]);
                if (x.caps & WCAP_IN_AMP) {
                    Amp a{n, false, x.type == W_SELECTOR || x.type == W_ADC ? p.idx[i] : static_cast<u8>(0), x.amp_in};
                    set_amp(a, -1, a.offset(), false);
                }
            }
        }
        if (i > 0 && (x.caps & WCAP_OUT_AMP)) {
            Amp a{n, true, 0, x.amp_out};
            set_amp(a, -1, a.offset(), false);
        }
    }
    u8 pin = p.nodes[p.len - 1];
    u8 ctl = PINCTL_IN;
    if (pin_device(w[pin]) == DEV_MIC && (w[pin].pincaps & PINCAP_VREF_80))
        ctl |= PINCTL_VREF_80; // bias voltage for electret microphones
    verb(pin, VERB_SET_PIN_CTL, ctl);
    if (w[pin].caps & WCAP_IN_AMP) {
        Amp a{pin, false, 0, w[pin].amp_in};
        set_amp(a, -1, a.offset(), false);
    }
}

// Find the gain amp (first with steps, from the converter side) and all
// mute-capable amps of a path.
void Controller::collect_amps(const Path& p, bool input, AmpGroup* g) {
    Amp candidates[MAX_PATH * 2];
    u32 n = 0;
    // Converter side first: output paths are pin..DAC, so walk them backwards.
    for (int k = 0; k < p.len; k++) {
        int i = input ? k : p.len - 1 - k;
        u8 nid = p.nodes[i];
        const Widget& x = w[nid];
        if (input) {
            if ((x.caps & WCAP_IN_AMP) && (i + 1 < p.len || x.type == W_PIN)) {
                u8 index = (i + 1 < p.len && x.type != W_PIN) ? p.idx[i] : 0;
                candidates[n++] = Amp{nid, false, index, x.amp_in};
            }
            if (i > 0 && (x.caps & WCAP_OUT_AMP))
                candidates[n++] = Amp{nid, true, 0, x.amp_out};
        } else {
            if (x.caps & WCAP_OUT_AMP)
                candidates[n++] = Amp{nid, true, 0, x.amp_out};
            if (i + 1 < p.len && x.type == W_MIXER && (x.caps & WCAP_IN_AMP))
                candidates[n++] = Amp{nid, false, p.idx[i], x.amp_in};
        }
    }
    bool have_volume = false;
    for (u32 i = 0; i < n; i++) {
        const Amp& a = candidates[i];
        if (!have_volume && a.steps() > 0 && g->nvolume < MAX_OUT_PATHS) {
            g->volume[g->nvolume++] = a;
            have_volume = true;
        }
        if (a.can_mute() && g->nmute < MAX_AMPS)
            g->mute[g->nmute++] = a;
    }
}

// Master is a virtual control on top of PCM: hw gain = pcm - (max - master),
// i.e. both attenuate in the same dB steps, like Linux's vmaster.
void Controller::apply_output_volume() {
    for (u32 i = 0; i < out_amps.nvolume; i++) {
        const Amp& a = out_amps.volume[i];
        for (int ch = 0; ch < 2; ch++) {
            i64 v = pcm[ch] - (vol_steps - master[ch]);
            if (v < 0)
                v = 0;
            u8 gain = vol_steps ? static_cast<u8>(v * a.steps() / vol_steps) : 0;
            bool muted = !master_on[ch];
            // An amp that can't mute is silenced by its lowest gain instead.
            set_amp(a, ch, muted && !a.can_mute() ? 0 : gain, muted && a.can_mute());
        }
    }
    for (u32 i = 0; i < out_amps.nmute; i++) {
        const Amp& a = out_amps.mute[i];
        bool is_volume = false;
        for (u32 k = 0; k < out_amps.nvolume; k++)
            is_volume |= out_amps.volume[k].nid == a.nid && out_amps.volume[k].output == a.output &&
                         out_amps.volume[k].index == a.index;
        if (is_volume)
            continue;
        for (int ch = 0; ch < 2; ch++)
            set_amp(a, ch, a.offset(), !master_on[ch]);
    }
}

void Controller::apply_capture_volume() {
    for (u32 i = 0; i < in_amps.nvolume; i++) {
        const Amp& a = in_amps.volume[i];
        for (int ch = 0; ch < 2; ch++)
            set_amp(a, ch, static_cast<u8>(capture[ch]), !capture_on[ch] && a.can_mute());
    }
    for (u32 i = 0; i < in_amps.nmute; i++) {
        const Amp& a = in_amps.mute[i];
        if (in_amps.nvolume && a.nid == in_amps.volume[0].nid && a.output == in_amps.volume[0].output)
            continue;
        for (int ch = 0; ch < 2; ch++)
            set_amp(a, ch, a.offset(), !capture_on[ch]);
    }
}

// ---- Mixer controls ----

Controller* ctl_hc(sound::Control* c) { return static_cast<Controller*>(c->driver_data); }

long get_master(sound::Control* c, i64* v) { v[0] = ctl_hc(c)->master[0]; v[1] = ctl_hc(c)->master[1]; return 0; }
long put_master(sound::Control* c, const i64* v) {
    Controller* hc = ctl_hc(c);
    if (hc->master[0] == v[0] && hc->master[1] == v[1]) return 0;
    hc->master[0] = v[0]; hc->master[1] = v[1];
    hc->apply_output_volume();
    return 1;
}
long get_pcm(sound::Control* c, i64* v) { v[0] = ctl_hc(c)->pcm[0]; v[1] = ctl_hc(c)->pcm[1]; return 0; }
long put_pcm(sound::Control* c, const i64* v) {
    Controller* hc = ctl_hc(c);
    if (hc->pcm[0] == v[0] && hc->pcm[1] == v[1]) return 0;
    hc->pcm[0] = v[0]; hc->pcm[1] = v[1];
    hc->apply_output_volume();
    return 1;
}
long get_master_sw(sound::Control* c, i64* v) { v[0] = ctl_hc(c)->master_on[0]; v[1] = ctl_hc(c)->master_on[1]; return 0; }
long put_master_sw(sound::Control* c, const i64* v) {
    Controller* hc = ctl_hc(c);
    if (hc->master_on[0] == (v[0] != 0) && hc->master_on[1] == (v[1] != 0)) return 0;
    hc->master_on[0] = v[0] != 0; hc->master_on[1] = v[1] != 0;
    hc->apply_output_volume();
    return 1;
}
long get_capture(sound::Control* c, i64* v) { v[0] = ctl_hc(c)->capture[0]; v[1] = ctl_hc(c)->capture[1]; return 0; }
long put_capture(sound::Control* c, const i64* v) {
    Controller* hc = ctl_hc(c);
    if (hc->capture[0] == v[0] && hc->capture[1] == v[1]) return 0;
    hc->capture[0] = v[0]; hc->capture[1] = v[1];
    hc->apply_capture_volume();
    return 1;
}
long get_capture_sw(sound::Control* c, i64* v) { v[0] = ctl_hc(c)->capture_on[0]; v[1] = ctl_hc(c)->capture_on[1]; return 0; }
long put_capture_sw(sound::Control* c, const i64* v) {
    Controller* hc = ctl_hc(c);
    if (hc->capture_on[0] == (v[0] != 0) && hc->capture_on[1] == (v[1] != 0)) return 0;
    hc->capture_on[0] = v[0] != 0; hc->capture_on[1] = v[1] != 0;
    hc->apply_capture_volume();
    return 1;
}

sound::Control make_control(const char* name, bool is_switch, u8 steps, const Amp* db_amp, Controller* hc,
                            long (*get)(sound::Control*, i64*), long (*put)(sound::Control*, const i64*)) {
    sound::Control c;
    sound::copy_name(c.name, name, sizeof(c.name));
    c.count = 2;
    if (is_switch) {
        c.type = sound::abi::ELEM_TYPE_BOOLEAN;
        c.max = 1;
    } else {
        c.type = sound::abi::ELEM_TYPE_INTEGER;
        c.max = steps;
        if (db_amp) {
            c.has_db = true;
            c.db_step = static_cast<i32>(db_amp->step_cdb());
            c.db_min = -static_cast<i32>(db_amp->offset()) * c.db_step;
            c.db_mute_at_min = false;
        }
    }
    c.get = get;
    c.put = put;
    c.driver_data = hc;
    return c;
}

void Controller::setup_mixer() {
    for (u32 i = 0; i < nout; i++)
        collect_amps(out_paths[i], false, &out_amps);
    if (has_input)
        collect_amps(in_path, true, &in_amps);

    if (out_amps.nvolume) {
        const Amp& a = out_amps.volume[0];
        vol_steps = a.steps();
        master[0] = master[1] = vol_steps;                      // Master fully open...
        pcm[0] = pcm[1] = min<u8>(a.offset(), vol_steps);       // ...and PCM at 0 dB
        // Master's dB scale: 0 dB at the top, same step as the amp.
        Amp master_db = a;
        master_db.caps = (a.caps & ~0x7Fu) | vol_steps;
        sound::control_add(card, make_control("Master Playback Volume", false, vol_steps, &master_db, this, get_master, put_master));
        sound::control_add(card, make_control("PCM Playback Volume", false, vol_steps, &a, this, get_pcm, put_pcm));
    }
    if (nout)
        sound::control_add(card, make_control("Master Playback Switch", true, 1, nullptr, this, get_master_sw, put_master_sw));
    apply_output_volume();

    if (in_amps.nvolume) {
        const Amp& a = in_amps.volume[0];
        cap_steps = a.steps();
        capture[0] = capture[1] = min<u8>(a.offset(), cap_steps);
        sound::control_add(card, make_control("Capture Volume", false, cap_steps, &a, this, get_capture, put_capture));
    }
    if (has_input)
        sound::control_add(card, make_control("Capture Switch", true, 1, nullptr, this, get_capture_sw, put_capture_sw));
    if (has_input)
        apply_capture_volume();
}

// ---- Streams ----

constexpr u32 RATE_TABLE[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};

// HDA stream format word for a rate (base 48/44.1 kHz, multiplier, divisor).
u16 rate_bits(u32 rate) {
    switch (rate) {
    case 8000: return (0 << 14) | (0 << 11) | (5 << 8);   // 48k / 6
    case 11025: return (1 << 14) | (0 << 11) | (3 << 8);  // 44.1k / 4
    case 16000: return (0 << 14) | (0 << 11) | (2 << 8);  // 48k / 3
    case 22050: return (1 << 14) | (0 << 11) | (1 << 8);  // 44.1k / 2
    case 32000: return (0 << 14) | (1 << 11) | (2 << 8);  // 48k * 2 / 3
    case 44100: return (1 << 14);
    case 88200: return (1 << 14) | (1 << 11);
    case 96000: return (0 << 14) | (1 << 11);
    case 176400: return (1 << 14) | (3 << 11);
    case 192000: return (0 << 14) | (3 << 11);
    default: return 0;                                    // 48 kHz
    }
}

sound::PcmHardware Controller::hardware_for(u8 converter, bool capture_dir) {
    sound::PcmHardware caps;
    const Widget& x = w[converter];
    u32 pcm_caps = x.pcm;
    for (u32 i = 0; i < array_size(RATE_TABLE); i++)
        if (pcm_caps & (1u << i))
            caps.rates[caps.rate_count++] = RATE_TABLE[i];
    if (!caps.rate_count) { // nonsense caps: everything supports 48 kHz
        caps.rates[0] = 48000;
        caps.rate_count = 1;
    }
    if (pcm_caps & (1u << 17))
        caps.formats |= 1ull << sound::abi::FORMAT_S16_LE;
    if (pcm_caps & ((1u << 18) | (1u << 19) | (1u << 20))) {
        caps.formats |= 1ull << sound::abi::FORMAT_S32_LE; // 20/24/32-bit samples in 32-bit containers
        caps.msbits = (pcm_caps & (1u << 20)) ? 32 : (pcm_caps & (1u << 19)) ? 24 : 20;
    }
    if (!caps.formats)
        caps.formats = 1ull << sound::abi::FORMAT_S16_LE;
    u32 chans = ((((x.caps >> 13) & 7) << 1) | (x.caps & WCAP_STEREO)) + 1;
    caps.channels_min = capture_dir ? 1 : 2;
    caps.channels_max = max<u32>(2, chans);
    caps.byte_align = 128;           // BDL entries must be 128-byte multiples
    caps.period_bytes_min = 128;
    caps.periods_min = 2;
    caps.periods_max = 32;
    caps.info = sound::abi::INFO_INTERLEAVED | sound::abi::INFO_MMAP | sound::abi::INFO_MMAP_VALID |
              sound::abi::INFO_BLOCK_TRANSFER | sound::abi::INFO_PAUSE;
    return caps;
}

Stream* stream_of(sound::Substream* s) { return static_cast<Stream*>(s->driver_data); }

void stream_reset(Controller* hc, Stream& st) {
    u32 base = hc->sd_base(st.sd);
    hc->wr<u32>(base + SD_CTL, hc->rd<u32>(base + SD_CTL) & ~(SD_CTL_RUN | SD_CTL_IOCE | SD_CTL_FEIE | SD_CTL_DEIE));
    wait_for([&] { return (hc->rd<u32>(base + SD_CTL) & SD_CTL_RUN) == 0; }, 10'000'000);
    hc->wr<u8>(base + SD_STS, SD_STS_MASK);
    hc->wr<u32>(base + SD_CTL, hc->rd<u32>(base + SD_CTL) | SD_CTL_SRST);
    wait_for([&] { return (hc->rd<u32>(base + SD_CTL) & SD_CTL_SRST) != 0; }, 10'000'000);
    hc->wr<u32>(base + SD_CTL, hc->rd<u32>(base + SD_CTL) & ~SD_CTL_SRST);
    wait_for([&] { return (hc->rd<u32>(base + SD_CTL) & SD_CTL_SRST) == 0; }, 10'000'000);
}

long op_prepare(sound::Substream* s) {
    Stream& st = *stream_of(s);
    Controller* hc = st.hc;
    const sound::PcmConfig& cfg = s->cfg;
    u32 period_bytes = static_cast<u32>(cfg.period_size * cfg.frame_bytes);
    u32 buffer_bytes = static_cast<u32>(cfg.buffer_size * cfg.frame_bytes);
    if (cfg.periods > 256 || period_bytes % 128)
        return -err::INVAL;

    u16 bits;
    if (cfg.format == sound::abi::FORMAT_S16_LE)
        bits = 1;
    else
        bits = s->hw.msbits == 32 ? 4 : s->hw.msbits == 24 ? 3 : 2;
    st.format = static_cast<u16>(rate_bits(cfg.rate) | (bits << 4) | ((cfg.channels - 1) & 0xF));

    stream_reset(hc, st);
    for (u64 i = 0; i < cfg.periods; i++) {
        st.bdl[i].address = s->dma_phys + i * period_bytes;
        st.bdl[i].length = period_bytes;
        st.bdl[i].flags = 1; // interrupt when this period is done
    }
    u32 base = hc->sd_base(st.sd);
    hc->wr<u32>(base + SD_BDPL, static_cast<u32>(st.bdl_phys));
    hc->wr<u32>(base + SD_BDPU, static_cast<u32>(st.bdl_phys >> 32));
    hc->wr<u32>(base + SD_CBL, buffer_bytes);
    hc->wr<u16>(base + SD_LVI, static_cast<u16>(cfg.periods - 1));
    hc->wr<u16>(base + SD_FMT, st.format);
    u32 ctl = hc->rd<u32>(base + SD_CTL) & ~(0xFu << 20);
    hc->wr<u32>(base + SD_CTL, ctl | (static_cast<u32>(st.tag) << 20));

    // Point the converters at the stream.
    for (u32 i = 0; i < st.nconverters; i++) {
        u8 nid = st.converters[i];
        hc->verb(nid, VERB_SET_FORMAT, st.format);
        if (cfg.channels > 2)
            hc->verb(nid, VERB_SET_CHANNEL_COUNT, cfg.channels - 1);
        hc->verb(nid, VERB_SET_STREAM, static_cast<u32>(st.tag) << 4);
    }
    return 0;
}

long op_trigger(sound::Substream* s, int cmd) {
    Stream& st = *stream_of(s);
    Controller* hc = st.hc;
    u32 base = hc->sd_base(st.sd);
    u32 ctl = hc->rd<u32>(base + SD_CTL);
    switch (cmd) {
    case sound::TRIGGER_START:
    case sound::TRIGGER_PAUSE_RELEASE:
        hc->wr<u32>(INTCTL, hc->rd<u32>(INTCTL) | (1u << st.sd) | INTCTL_GIE);
        hc->wr<u32>(base + SD_CTL, ctl | SD_CTL_RUN | SD_CTL_IOCE | SD_CTL_DEIE);
        st.running = true;
        return 0;
    case sound::TRIGGER_STOP:
    case sound::TRIGGER_PAUSE_PUSH:
        hc->wr<u32>(base + SD_CTL, ctl & ~(SD_CTL_RUN | SD_CTL_IOCE | SD_CTL_DEIE));
        wait_for([&] { return (hc->rd<u32>(base + SD_CTL) & SD_CTL_RUN) == 0; }, 2'000'000);
        hc->wr<u8>(base + SD_STS, SD_STS_MASK);
        hc->wr<u32>(INTCTL, hc->rd<u32>(INTCTL) & ~(1u << st.sd));
        st.running = false;
        return 0;
    default:
        return -err::INVAL;
    }
}

u64 op_pointer(sound::Substream* s) {
    Stream& st = *stream_of(s);
    u32 lpib = st.hc->rd<u32>(st.hc->sd_base(st.sd) + SD_LPIB);
    u32 bytes = static_cast<u32>(s->cfg.buffer_size * s->cfg.frame_bytes);
    if (lpib >= bytes)
        lpib = 0;
    return lpib / s->cfg.frame_bytes;
}

void op_hw_free(sound::Substream* s) {
    Stream& st = *stream_of(s);
    for (u32 i = 0; i < st.nconverters; i++)
        st.hc->verb(st.converters[i], VERB_SET_STREAM, 0); // detach the converters
}

constexpr sound::PcmOps g_ops{op_prepare, op_trigger, op_pointer, op_hw_free};

bool Controller::setup_stream(Stream& st, u32 sd, u8 tag, bool capture_dir) {
    st.hc = this;
    st.sd = sd;
    st.tag = tag;
    st.capture = capture_dir;
    st.bdl_phys = pmm::alloc_page(true); // 256 entries * 16 bytes = one page, 128-byte aligned
    if (!st.bdl_phys)
        return false;
    st.bdl = mm::phys_to_virt<BdlEntry>(st.bdl_phys);
    return true;
}

// ---- Interrupts ----

Controller* g_controllers[4];
u32 g_controller_count = 0;

void irq(interrupts::Frame*) {
    for (u32 i = 0; i < g_controller_count; i++) {
        Controller* hc = g_controllers[i];
        u32 sts = hc->rd<u32>(INTSTS);
        if (!sts || sts == 0xFFFFFFFF)
            continue;
        for (u32 sd = 0; sd < hc->iss + hc->oss && sd < 30; sd++)
            if (sts & (1u << sd))
                hc->wr<u8>(hc->sd_base(sd) + SD_STS, SD_STS_MASK); // acknowledge
        if (sts & INTCTL_CIE)
            hc->wr<u8>(RIRBSTS, 0x5);
        hc->irq_signal.signal();
    }
}

// Period interrupts land here (outside interrupt context) and drive the sound core.
void irq_thread(void* arg) {
    auto* hc = static_cast<Controller*>(arg);
    for (;;) {
        hc->irq_signal.wait_timeout(100'000'000);
        if (hc->play.running && hc->play.sub)
            sound::period_elapsed(hc->play.sub);
        if (hc->cap.running && hc->cap.sub)
            sound::period_elapsed(hc->cap.sub);
    }
}

// ---- /proc/asound/cardN/codec#0 ----

const char* type_name(u8 t) {
    switch (t) {
    case W_DAC: return "Audio Output";
    case W_ADC: return "Audio Input";
    case W_MIXER: return "Audio Mixer";
    case W_SELECTOR: return "Audio Selector";
    case W_PIN: return "Pin Complex";
    case W_POWER: return "Power Widget";
    case W_KNOB: return "Volume Knob Widget";
    case W_BEEP: return "Beep Generator Widget";
    default: return "Vendor Defined Widget";
    }
}

bool gen_codec(genfs::TextBuffer& out, const void* ctx) {
    auto* hc = static_cast<const Controller*>(ctx);
    out.printf("Codec: %04x:%04x\nAddress: %u\nVendor Id: 0x%08x\nAFG: 0x%02x\n", hc->vendor_id >> 16,
               hc->vendor_id & 0xFFFF, hc->codec, hc->vendor_id, hc->afg);
    for (u32 n = 0; n < MAX_NID; n++) {
        const Widget& x = hc->w[n];
        if (!x.present)
            continue;
        out.printf("Node 0x%02x [%s] wcaps 0x%x:%s%s%s%s\n", n, type_name(x.type), x.caps,
                   (x.caps & WCAP_STEREO) ? " Stereo" : " Mono", (x.caps & WCAP_IN_AMP) ? " Amp-In" : "",
                   (x.caps & WCAP_OUT_AMP) ? " Amp-Out" : "", (x.caps & WCAP_DIGITAL) ? " Digital" : "");
        if (x.amp_in)
            out.printf("  Amp-In caps: ofs=0x%02x, nsteps=0x%02x, stepsize=0x%02x, mute=%u\n", x.amp_in & 0x7F,
                       (x.amp_in >> 8) & 0x7F, (x.amp_in >> 16) & 0x7F, x.amp_in >> 31);
        if (x.amp_out)
            out.printf("  Amp-Out caps: ofs=0x%02x, nsteps=0x%02x, stepsize=0x%02x, mute=%u\n", x.amp_out & 0x7F,
                       (x.amp_out >> 8) & 0x7F, (x.amp_out >> 16) & 0x7F, x.amp_out >> 31);
        if (x.type == W_DAC || x.type == W_ADC)
            out.printf("  PCM: rates 0x%03x, bits 0x%02x, formats 0x%x\n", x.pcm & 0xFFF, (x.pcm >> 16) & 0x1F,
                       x.formats);
        if (x.type == W_PIN)
            out.printf("  Pincap 0x%08x, Pin Default 0x%08x (device %u, connectivity %u)\n", x.pincaps, x.config,
                       pin_device(x), pin_connectivity(x));
        if (x.nconn) {
            out.printf("  Connection: %u\n    ", x.nconn);
            for (u8 i = 0; i < x.nconn; i++)
                out.printf(" 0x%02x", x.conn[i]);
            out.append("\n");
        }
    }
    auto print_path = [&](const char* what, const Path& p) {
        out.printf("%s path:", what);
        for (u8 i = 0; i < p.len; i++)
            out.printf(" 0x%02x", p.nodes[i]);
        out.append("\n");
    };
    for (u32 i = 0; i < hc->nout; i++)
        print_path("Output", hc->out_paths[i]);
    if (hc->has_input)
        print_path("Input", hc->in_path);
    return true;
}

// ---- Probe ----

const char* controller_name(u16 vendor) {
    switch (vendor) {
    case 0x8086: return "Intel";
    case 0x1022: case 0x1002: return "AMD";
    case 0x10de: return "NVIDIA";
    case 0x1106: return "VIA";
    default: return "Generic";
    }
}

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && (d->class_code >> 8) == 0x0403; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    if (d->vendor == 0x8086) {
        // Intel: DMA must snoop the CPU caches, or the controller plays stale
        // memory while fresh samples sit in the cache (crackling). Traffic
        // class 0, and clear the "no snoop" bit firmware may leave set (Linux
        // does both for every Intel HDA controller).
        pci::write8(*f, 0x44, pci::read8(*f, 0x44) & ~0x07);
        u16 devc = pci::read16(*f, 0x78);
        if (devc & (1u << 11)) {
            pci::write16(*f, 0x78, devc & ~(1u << 11));
            klog::info("hda %s: snooping turned on (firmware left it off)", d->bus_id);
        }
    }
    // Snooped DMA: Intel's traffic-class select must be TC0, and ATI/AMD
    // controllers have an explicit "enable snoop" bit; otherwise audio
    // reads stale cache contents on real hardware.
    if (d->vendor == 0x8086)
        pci::write8(*f, 0x44, pci::read8(*f, 0x44) & ~7);
    if (d->vendor == 0x1002 || d->vendor == 0x1022)
        pci::write8(*f, 0x42, static_cast<u8>((pci::read8(*f, 0x42) & ~7) | 2));

    auto* hc = new Controller;
    hc->pci = f;
    hc->hw = d;
    hc->regs = static_cast<volatile u8*>(pci::map_bar(*f, 0));
    if (!hc->regs)
        return -err::IO;
    u16 gcap = hc->rd<u16>(GCAP);
    hc->iss = (gcap >> 8) & 0xF;
    hc->oss = (gcap >> 12) & 0xF;
    if (!hc->reset()) {
        klog::warn("hda %s: controller reset failed", d->bus_id);
        return -err::IO;
    }
    u16 codecs = hc->rd<u16>(STATESTS);
    if (!hc->setup_rings())
        return -err::NOMEM;

    // Drive the first codec with an analog audio function group.
    bool found = false;
    for (u8 addr = 0; addr < 15 && !found; addr++) {
        if (!(codecs & (1u << addr)))
            continue;
        if (!hc->probe_codec(addr))
            continue;
        // Output jacks: line outs first (they're "the" output), then headphones, speakers.
        static constexpr u8 order[] = {DEV_LINE_OUT, DEV_HP_OUT, DEV_SPEAKER};
        u8 dacs[MAX_OUT_PATHS];
        u32 ndacs = 0;
        for (u8 want : order) {
            for (u32 n = 0; n < MAX_NID && hc->nout < MAX_OUT_PATHS; n++) {
                const Widget& x = hc->w[n];
                if (!x.present || x.type != W_PIN || !(x.pincaps & PINCAP_OUT) || (x.caps & WCAP_DIGITAL) ||
                    pin_connectivity(x) == CONN_NONE || pin_device(x) != want)
                    continue;
                Path p;
                if (!hc->find_path(static_cast<u8>(n), true, dacs, ndacs, &p))
                    continue;
                hc->out_paths[hc->nout++] = p;
                u8 dac = p.nodes[p.len - 1];
                bool known = false;
                for (u32 i = 0; i < ndacs; i++)
                    known |= dacs[i] == dac;
                if (!known)
                    dacs[ndacs++] = dac;
            }
        }
        // Input: from an ADC to the microphone (or line-in).
        for (u32 n = 0; n < MAX_NID && !hc->has_input; n++) {
            const Widget& x = hc->w[n];
            if (x.present && x.type == W_ADC && !(x.caps & WCAP_DIGITAL) && hc->find_path(static_cast<u8>(n), false, nullptr, 0, &hc->in_path))
                hc->has_input = true;
        }
        if (hc->nout || hc->has_input) {
            found = true;
            for (u32 i = 0; i < ndacs; i++)
                hc->play.converters[hc->play.nconverters++] = dacs[i];
        } else {
            hc->nout = 0;
        }
    }
    if (!found) {
        klog::info("hda %s: no analog codec (HDMI-only or none); not creating a sound card", d->bus_id);
        return -err::NODEV;
    }
    if (hc->has_input) {
        hc->cap.converters[0] = hc->in_path.nodes[0];
        hc->cap.nconverters = 1;
    }
    for (u32 i = 0; i < hc->nout; i++) {
        const Path& p = hc->out_paths[i];
        hc->enable_output_path(p, pin_device(hc->w[p.nodes[0]]) == DEV_HP_OUT);
    }
    if (hc->has_input)
        hc->enable_input_path(hc->in_path);
    hc->silence_unused();
    hc->log_codec();
    hc->start_jack_sense();

    // Card and PCM.
    char longname[80];
    snprintf(longname, sizeof(longname), "HDA %s at 0x%lx irq %u", controller_name(d->vendor),
             f->bars[0].address, static_cast<unsigned>(f->irq_line));
    char shortname[32];
    snprintf(shortname, sizeof(shortname), "HDA %s", controller_name(d->vendor));
    hc->card = sound::card_create(d->vendor == 0x8086 ? "Intel" : controller_name(d->vendor), "HDA-Intel", shortname,
                                  longname, d);
    if (!hc->card)
        return -err::NOMEM;
    snprintf(hc->card->mixername, sizeof(hc->card->mixername), "Codec %04x:%04x", hc->vendor_id >> 16,
             hc->vendor_id & 0xFFFF);
    snprintf(hc->card->components, sizeof(hc->card->components), "HDA:%08x", hc->vendor_id);

    sound::Pcm* pcm = sound::pcm_create(hc->card, "HDA Analog", hc->nout > 0 && hc->oss > 0,
                                        hc->has_input && hc->iss > 0);
    if (!pcm)
        return -err::NOMEM;
    if (pcm->streams[0]) {
        if (!hc->setup_stream(hc->play, hc->iss, 1, false))
            return -err::NOMEM;
        hc->play.sub = pcm->streams[0];
        if (!sound::pcm_set_ops(pcm, sound::abi::STREAM_PLAYBACK, hc->hardware_for(hc->play.converters[0], false), &g_ops,
                                &hc->play))
            return -err::NOMEM;
    }
    if (pcm->streams[1]) {
        if (!hc->setup_stream(hc->cap, 0, 2, true))
            return -err::NOMEM;
        hc->cap.sub = pcm->streams[1];
        if (!sound::pcm_set_ops(pcm, sound::abi::STREAM_CAPTURE, hc->hardware_for(hc->cap.converters[0], true), &g_ops,
                                &hc->cap))
            return -err::NOMEM;
    }
    hc->setup_mixer();
    sound::card_proc_file(hc->card, "codec#0", gen_codec, hc);

    u8 vector = interrupts::allocate_vector();
    bool msi = vector && g_controller_count < array_size(g_controllers) && pci::enable_msi(*f, vector);
    if (msi) {
        g_controllers[g_controller_count++] = hc;
        interrupts::set_handler(vector, irq);
        hc->wr<u32>(INTCTL, INTCTL_GIE);
    }
    sched::spawn("hda-irq", irq_thread, hc, -1, true);

    snprintf(d->name, sizeof(d->name), "%s High Definition Audio Controller", controller_name(d->vendor));
    sound::card_register(hc->card);
    klog::info("hda %s: codec %04x:%04x (HDA %u.%u), %u output path(s)%s%s", d->bus_id, hc->vendor_id >> 16,
               hc->vendor_id & 0xFFFF, hc->rd<u8>(VMAJ), hc->rd<u8>(VMIN), hc->nout,
               hc->has_input ? ", capture" : "", msi ? ", MSI" : ", polled");
    return 0;
}

dev::Driver g_driver{"snd_hda_intel", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace hda
