// USB Audio Class 1 playback (headsets, USB DACs/speakers, docks): the
// device's AudioStreaming interface has alternate settings, each with an
// isochronous OUT endpoint and a format (channels, sample size, rates).
// Choosing a setting + setting the sample rate starts the stream; every
// 1 ms packet then carries rate/1000 frames from the ALSA ring buffer.

#include <core/log.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <sched/sched.hpp>
#include <sound/sound.hpp>

namespace usb::audio {

namespace {

namespace abi = sound::abi;

struct Alt {
    u8 intf = 0, alt = 0;
    u8 ep = 0;
    u16 max_packet = 0;
    u8 channels = 0, subframe = 0, bits = 0;
    u32 rates[12] = {};
    u32 nrates = 0;
    bool continuous = false;
    u32 rate_min = 0, rate_max = 0;
    u16 format_tag = 0;
};

struct Uac {
    Device* d = nullptr;
    Alt alt;
    sound::Substream* sub = nullptr;
    bool ep_open = false;
    bool streaming = false;
    volatile bool running = false;
    u32 rate = 48000, frame_bytes = 4, acc = 0;
    u64 pos = 0;            // frames into the ring buffer
    u64 buffer_frames = 0, period_frames = 0, period_acc = 0;
    u32 periods_pending = 0; // period boundaries for the worker to report
    sched::WaitQueue wake;
};

u32 rate3(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16); }

// Walk the configuration descriptor for playback alternate settings.
bool find_playback(const Device* d, Alt* best) {
    const u8* c = d->config_desc;
    u32 len = d->config_len;
    Alt cur;
    bool in_as = false, found = false;
    auto consider = [&](const Alt& a) {
        if (!a.ep || a.format_tag != 1 /* PCM */ || a.subframe != 2 || !a.channels)
            return;
        // Prefer stereo; otherwise the first one.
        if (!found || (a.channels == 2 && best->channels != 2)) {
            *best = a;
            found = true;
        }
    };
    for (u32 i = 0; i + 2 <= len && c[i] >= 2; i += c[i]) {
        const u8* x = c + i;
        u8 type = x[1];
        if (type == 4 && x[0] >= 9) { // interface
            if (in_as)
                consider(cur);
            cur = Alt{};
            cur.intf = x[2];
            cur.alt = x[3];
            in_as = x[5] == 1 && x[6] == 2; // audio / streaming
        } else if (in_as && type == 0x24 && x[0] >= 3) { // class-specific interface
            if (x[2] == 1 && x[0] >= 7) { // AS_GENERAL
                cur.format_tag = static_cast<u16>(x[5] | (x[6] << 8));
            } else if (x[2] == 2 && x[0] >= 8 && x[3] == 1) { // FORMAT_TYPE I
                cur.channels = x[4];
                cur.subframe = x[5];
                cur.bits = x[6];
                u8 n = x[7];
                if (n == 0 && x[0] >= 14) {
                    cur.continuous = true;
                    cur.rate_min = rate3(x + 8);
                    cur.rate_max = rate3(x + 11);
                } else {
                    for (u8 k = 0; k < n && cur.nrates < 12 && 8u + 3u * k + 3u <= x[0]; k++)
                        cur.rates[cur.nrates++] = rate3(x + 8 + 3 * k);
                }
            }
        } else if (in_as && type == 5 && x[0] >= 7) { // endpoint
            if ((x[3] & 3) == 1 && !(x[2] & 0x80) && !cur.ep) { // isochronous OUT
                cur.ep = x[2];
                cur.max_packet = static_cast<u16>((x[4] | (x[5] << 8)) & 0x7FF);
            }
        }
    }
    if (in_as)
        consider(cur);
    return found;
}

u32 fill(void* ctx, u8* buf, u32 max) {
    auto* u = static_cast<Uac*>(ctx);
    u32 frames = u->rate / 1000;
    u->acc += u->rate % 1000;
    if (u->acc >= 1000) {
        u->acc -= 1000;
        frames++;
    }
    u32 bytes = frames * u->frame_bytes;
    if (bytes > max) {
        frames = max / u->frame_bytes;
        bytes = frames * u->frame_bytes;
    }
    if (!u->running || !u->buffer_frames) {
        memset(buf, 0, bytes);
        return bytes;
    }
    const u8* ring = u->sub->dma_area;
    u64 pos = u->pos;
    for (u32 done = 0; done < frames;) {
        u32 chunk = static_cast<u32>(min<u64>(frames - done, u->buffer_frames - pos));
        memcpy(buf + done * u->frame_bytes, ring + pos * u->frame_bytes, chunk * u->frame_bytes);
        done += chunk;
        pos = (pos + chunk) % u->buffer_frames;
    }
    __atomic_store_n(&u->pos, pos, __ATOMIC_RELEASE);
    u->period_acc += frames;
    bool boundary = false;
    while (u->period_acc >= u->period_frames) {
        u->period_acc -= u->period_frames;
        __atomic_add_fetch(&u->periods_pending, 1, __ATOMIC_RELEASE);
        boundary = true;
    }
    if (boundary)
        u->wake.wake_all();
    return bytes;
}

// period_elapsed may sleep: report boundaries from a thread, not from the
// host controller's event path.
void worker(void* arg) {
    auto* u = static_cast<Uac*>(arg);
    for (;;) {
        u->wake.wait_until([u] { return __atomic_load_n(&u->periods_pending, __ATOMIC_ACQUIRE) != 0; });
        u32 n = __atomic_exchange_n(&u->periods_pending, 0u, __ATOMIC_ACQ_REL);
        while (n--)
            sound::period_elapsed(u->sub);
    }
}

void stop_stream(Uac* u) {
    if (!u->streaming)
        return;
    u->d->stop_iso(u->alt.ep);
    u->streaming = false;
}

long op_prepare(sound::Substream* s) {
    auto* u = static_cast<Uac*>(s->driver_data);
    stop_stream(u);
    u->running = false;
    Device* d = u->d;
    if (d->control(0x01, 11 /* SET_INTERFACE */, u->alt.alt, u->alt.intf, nullptr, 0) < 0)
        return -err::IO;
    if (!u->ep_open) {
        Endpoint ep{u->alt.ep, 1, u->alt.max_packet, 1};
        if (d->open_endpoint(ep) != 0)
            return -err::IO;
        u->ep_open = true;
    }
    u->rate = s->cfg.rate;
    if (u->alt.continuous || u->alt.nrates > 1) {
        u8 r[3] = {static_cast<u8>(u->rate), static_cast<u8>(u->rate >> 8), static_cast<u8>(u->rate >> 16)};
        d->control(0x22, 0x01 /* SET_CUR */, 0x0100 /* sampling frequency */, u->alt.ep, r, 3);
    }
    u->frame_bytes = s->cfg.frame_bytes;
    u->buffer_frames = s->cfg.buffer_size;
    u->period_frames = s->cfg.period_size ? s->cfg.period_size : 1;
    u->pos = 0;
    u->acc = 0;
    u->period_acc = 0;
    __atomic_store_n(&u->periods_pending, 0u, __ATOMIC_RELEASE);
    if (d->start_iso_out(u->alt.ep, u->alt.max_packet, fill, u) < 0)
        return -err::IO;
    u->streaming = true; // silence until started
    return 0;
}

long op_trigger(sound::Substream* s, int cmd) {
    auto* u = static_cast<Uac*>(s->driver_data);
    if (cmd == sound::TRIGGER_START || cmd == sound::TRIGGER_PAUSE_RELEASE)
        u->running = true;
    else if (cmd == sound::TRIGGER_STOP || cmd == sound::TRIGGER_PAUSE_PUSH)
        u->running = false;
    else
        return -err::INVAL;
    return 0;
}

u64 op_pointer(sound::Substream* s) {
    auto* u = static_cast<Uac*>(s->driver_data);
    return __atomic_load_n(&u->pos, __ATOMIC_ACQUIRE);
}

void op_hw_free(sound::Substream* s) {
    auto* u = static_cast<Uac*>(s->driver_data);
    u->running = false;
    stop_stream(u);
    u->d->control(0x01, 11, 0, u->alt.intf, nullptr, 0); // zero-bandwidth setting
}

constexpr sound::PcmOps g_ops{op_prepare, op_trigger, op_pointer, op_hw_free};

} // namespace

bool probe(Device* d, const Interface& intf) {
    if (intf.cls != 1 || intf.subclass != 1) // the AudioControl interface stands for the function
        return false;
    Alt alt;
    if (!find_playback(d, &alt))
        return false;
    if (!d->entry)
        return false;
    auto* u = new Uac;
    u->d = d;
    u->alt = alt;
    char longname[80];
    snprintf(longname, sizeof(longname), "%s at usb-%s", d->product_name, d->path);
    sound::Card* card = sound::card_create("USB", "USB-Audio", d->product_name, longname, d->entry);
    if (!card)
        return false;
    sound::Pcm* pcm = sound::pcm_create(card, "USB Audio", true, false);
    if (!pcm)
        return false;
    sound::PcmHardware hw;
    hw.formats = 1ull << abi::FORMAT_S16_LE;
    hw.channels_min = hw.channels_max = alt.channels;
    if (alt.continuous) {
        static const u32 standard[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000};
        for (u32 r : standard)
            if (r >= alt.rate_min && r <= alt.rate_max && hw.rate_count < 16)
                hw.rates[hw.rate_count++] = r;
    } else {
        for (u32 i = 0; i < alt.nrates; i++)
            hw.rates[hw.rate_count++] = alt.rates[i];
    }
    if (!hw.rate_count)
        return false;
    hw.info &= ~abi::INFO_BLOCK_TRANSFER;
    if (!sound::pcm_set_ops(pcm, abi::STREAM_PLAYBACK, hw, &g_ops, u))
        return false;
    u->sub = pcm->streams[abi::STREAM_PLAYBACK];
    sched::spawn("usb-audio", worker, u, -1, true);
    sound::card_register(card);
    klog::ok("usb-audio: %s: playback, %u channel(s), 16-bit, %u rate(s) (%u-%u Hz), interface %u alt %u",
             d->product_name, alt.channels, hw.rate_count, hw.rates[0], hw.rates[hw.rate_count - 1], alt.intf,
             alt.alt);
    return true;
}

} // namespace usb::audio
