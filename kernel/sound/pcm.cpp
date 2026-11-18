// PCM devices (/dev/snd/pcmC<card>D<dev>p / c): the ALSA PCM state machine
// and ioctl ABI on top of a driver's DMA ring buffer.
//
// Pointers work exactly like Linux's: hw_ptr (how far the hardware got) and
// appl_ptr (how far the application got) count frames and wrap at
// `boundary`, a large multiple of the buffer size, so "avail" and "delay"
// are plain subtractions. Both live in pages that libasound may mmap
// (status read-only, control read-write); without those mappings it uses
// SYNC_PTR to exchange the same values.

#include <core/log.hpp>
#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sound/sound.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace sound {

namespace {

using namespace abi;

constexpr u64 LONG_MAX_U = 0x7FFFFFFFFFFFFFFFull;
constexpr u64 TRANSFER_TIMEOUT_NS = 10'000'000'000ull; // Linux: "DMA or IRQ trouble?" after 10 s

Timespec to_timespec(u64 ns) { return Timespec{static_cast<i64>(ns / 1'000'000'000), static_cast<i64>(ns % 1'000'000'000)}; }

// Timestamps use the clock the application asked for (TTSTAMP): monotonic or wall clock.
Timespec now_ts(const Substream* s) {
    return to_timespec(s->tstamp_type == 0 ? rtc::realtime_ns() : clock::now_ns());
}

bool is_playback(const Substream* s) { return s->stream == STREAM_PLAYBACK; }
int state(const Substream* s) { return s->status->state; }

// ---- Pointer arithmetic (stream lock held) ----

u64 playback_avail(const Substream* s) {
    i64 avail = static_cast<i64>(s->status->hw_ptr + s->cfg.buffer_size) - static_cast<i64>(s->control->appl_ptr);
    if (avail < 0)
        avail += static_cast<i64>(s->boundary);
    else if (static_cast<u64>(avail) >= s->boundary)
        avail -= static_cast<i64>(s->boundary);
    return static_cast<u64>(avail);
}

u64 capture_avail(const Substream* s) {
    i64 avail = static_cast<i64>(s->status->hw_ptr) - static_cast<i64>(s->control->appl_ptr);
    if (avail < 0)
        avail += static_cast<i64>(s->boundary);
    return static_cast<u64>(avail);
}

u64 avail(const Substream* s) { return is_playback(s) ? playback_avail(s) : capture_avail(s); }

// Frames queued in the hardware's direction: still to be played / already captured but unread.
i64 hw_avail(const Substream* s) { return static_cast<i64>(s->cfg.buffer_size) - static_cast<i64>(avail(s)); }

void stop_locked(Substream* s, int new_state) {
    int st = state(s);
    if (st == STATE_RUNNING || st == STATE_DRAINING || st == STATE_PAUSED)
        s->ops->trigger(s, TRIGGER_STOP);
    s->status->state = new_state;
    s->trigger_tstamp = now_ts(s);
}

void xrun_locked(Substream* s) {
    stop_locked(s, STATE_XRUN);
    s->xruns++;
}

// Zero `frames` frames of the ring buffer starting at ring position `ptr`.
void silence(Substream* s, u64 ptr, u64 frames) {
    u64 bs = s->cfg.buffer_size;
    frames = min(frames, bs);
    u64 ofs = ptr % bs;
    while (frames) {
        u64 n = min(frames, bs - ofs);
        memset(s->dma_area + ofs * s->cfg.frame_bytes, 0, n * s->cfg.frame_bytes);
        frames -= n;
        ofs = 0;
    }
}

// Read the hardware position and advance hw_ptr; detect xruns and the end of
// a drain. Returns true if anything a waiter cares about changed.
bool update_locked(Substream* s) {
    int st = state(s);
    if (st != STATE_RUNNING && st != STATE_DRAINING)
        return false;
    u64 bs = s->cfg.buffer_size;
    u64 pos = s->ops->pointer(s);
    if (pos >= bs)
        pos = 0;
    u64 now = clock::now_ns();
    u64 old_hw = s->status->hw_ptr;
    u64 base = s->hw_ptr_base;
    if (pos < s->last_pos) { // wrapped around the ring
        base += bs;
        if (base >= s->boundary)
            base = 0;
    }
    u64 new_hw = base + pos;
    u64 delta = new_hw >= old_hw ? new_hw - old_hw : new_hw + s->boundary - old_hw;

    // The position alone can't tell how many times the ring wrapped. If we
    // haven't looked for a long time (more than half a buffer), use the
    // clock to count laps we missed, like Linux does without period IRQs.
    u64 elapsed = now - s->last_update_ns;
    u64 buffer_ns = bs * 1'000'000'000ull / s->cfg.rate;
    if (elapsed > buffer_ns / 2) {
        u64 expected = elapsed / 1000 * s->cfg.rate / 1'000'000;
        while (expected > delta + bs / 2 + bs) {
            delta += bs;
            base += bs;
            if (base >= s->boundary)
                base -= s->boundary;
            new_hw = base + pos;
        }
    }

    // Applications that never let the stream stop (stop_threshold =
    // boundary, e.g. PulseAudio) ask us to silence what was just played so
    // an underrun repeats silence instead of old audio.
    if (is_playback(s) && delta && s->silence_size >= s->boundary)
        silence(s, old_hw, delta);

    s->hw_ptr_base = base;
    s->status->hw_ptr = new_hw;
    s->status->tstamp = now_ts(s);
    s->last_pos = pos;
    s->last_update_ns = now;

    u64 a = avail(s);
    if (a > s->avail_max)
        s->avail_max = a;
    if (is_playback(s) && st == STATE_DRAINING) {
        if (a >= bs) { // everything the application wrote has been played
            stop_locked(s, STATE_SETUP);
            return true;
        }
    } else if (a >= s->stop_threshold) {
        xrun_locked(s);
        return true;
    }
    return delta != 0;
}

void wake(Substream* s) {
    s->wait.wake_all();
    poll::notify();
}

long start_locked(Substream* s) {
    if (state(s) != STATE_PREPARED)
        return -snderr::BADFD;
    if (is_playback(s) && hw_avail(s) <= 0)
        return -err::PIPE;
    long r = s->ops->trigger(s, TRIGGER_START);
    if (r < 0)
        return r;
    s->status->state = STATE_RUNNING;
    s->trigger_tstamp = now_ts(s);
    s->last_update_ns = clock::now_ns();
    internal::timer_kick();
    return 0;
}

// Linux's do_pcm_hwsync().
long hwsync_locked(Substream* s) {
    switch (state(s)) {
    case STATE_DRAINING:
        if (!is_playback(s))
            return -snderr::BADFD;
        [[fallthrough]];
    case STATE_RUNNING:
        update_locked(s);
        return state(s) == STATE_XRUN ? -err::PIPE : 0;
    case STATE_PREPARED:
    case STATE_PAUSED:
        return 0;
    case STATE_SUSPENDED:
        return -snderr::STRPIPE;
    case STATE_XRUN:
        return -err::PIPE;
    default:
        return -snderr::BADFD;
    }
}

// ---- Data transfer ----

// Copy `frames` frames between the user buffer(s) and the ring at ring position `ofs`.
// For non-interleaved access `chan_ptrs` holds one user pointer per channel.
long copy_frames(Substream* s, bool write, u64 user_buf, const u64* chan_ptrs, u64 user_frame, u64 ofs, u64 frames) {
    u32 fb = s->cfg.frame_bytes;
    u8* ring = s->dma_area + ofs * fb;
    if (!chan_ptrs) {
        void* user = reinterpret_cast<void*>(user_buf + user_frame * fb);
        return write ? uaccess::copy_from(ring, user, frames * fb) : uaccess::copy_to(user, ring, frames * fb);
    }
    // Non-interleaved: (de)interleave one channel at a time through a bounce buffer.
    u32 sb = s->cfg.sample_bits / 8;
    u8 bounce[512];
    u64 per_chunk = sizeof(bounce) / sb;
    for (u32 c = 0; c < s->cfg.channels; c++) {
        for (u64 done = 0; done < frames;) {
            u64 n = min(per_chunk, frames - done);
            if (write) {
                if (chan_ptrs[c]) {
                    if (long r = uaccess::copy_from(bounce, reinterpret_cast<void*>(chan_ptrs[c] + (user_frame + done) * sb), n * sb); r < 0)
                        return r;
                } else {
                    memset(bounce, 0, n * sb); // a missing channel plays silence
                }
                for (u64 i = 0; i < n; i++)
                    memcpy(ring + (done + i) * fb + c * sb, bounce + i * sb, sb);
            } else if (chan_ptrs[c]) {
                for (u64 i = 0; i < n; i++)
                    memcpy(bounce + i * sb, ring + (done + i) * fb + c * sb, sb);
                if (long r = uaccess::copy_to(reinterpret_cast<void*>(chan_ptrs[c] + (user_frame + done) * sb), bounce, n * sb); r < 0)
                    return r;
            }
            done += n;
        }
    }
    return 0;
}

bool transfer_state_ok(const Substream* s) {
    int st = state(s);
    return st == STATE_PREPARED || st == STATE_RUNNING || st == STATE_PAUSED ||
           (!is_playback(s) && st == STATE_DRAINING);
}

long transfer_state_error(const Substream* s) {
    switch (state(s)) {
    case STATE_XRUN: return -err::PIPE;
    case STATE_SUSPENDED: return -snderr::STRPIPE;
    default: return -snderr::BADFD;
    }
}

// The heart of WRITEI/READI/WRITEN/READN and write()/read(): move up to
// `frames` frames, sleeping while the ring is full (playback) or empty
// (capture) unless the file is non-blocking. Returns frames moved or -errno.
i64 transfer(Substream* s, vfs::File* f, bool write, u64 user_buf, const u64* chan_ptrs, u64 frames) {
    if (!s->configured || state(s) == STATE_OPEN)
        return -snderr::BADFD;
    bool noninterleaved = chan_ptrs != nullptr;
    int want = noninterleaved ? ACCESS_RW_NONINTERLEAVED : ACCESS_RW_INTERLEAVED;
    if (s->cfg.access != want)
        return -err::INVAL;
    if (frames == 0)
        return 0;
    bool nonblock = f->flags & vfs::O_NONBLOCK;
    u64 done = 0;
    long error = 0;

    s->wait.lock().lock();
    if (!transfer_state_ok(s)) {
        error = transfer_state_error(s);
        s->wait.lock().unlock();
        return error;
    }
    // Capture starts as soon as someone wants enough data.
    if (!write && state(s) == STATE_PREPARED && frames >= s->start_threshold) {
        if (long r = start_locked(s); r < 0) {
            s->wait.lock().unlock();
            return r;
        }
    }
    while (done < frames) {
        update_locked(s);
        if (!transfer_state_ok(s)) {
            error = transfer_state_error(s);
            break;
        }
        u64 a = avail(s);
        if (a == 0) {
            if (!write && state(s) == STATE_DRAINING) { // capture drained: no more data will come
                s->status->state = STATE_SETUP;
                break;
            }
            if (nonblock) {
                error = -err::AGAIN;
                break;
            }
            // Wait for room / data. Wake up once at least min(remaining, avail_min) frames are available.
            u64 twake = min(frames - done, s->control->avail_min ? s->control->avail_min : 1);
            s->wait.lock().unlock();
            bool ok = s->wait.wait_until_timeout([&] {
                update_locked(s);
                return !transfer_state_ok(s) || (!write && state(s) == STATE_DRAINING) || avail(s) >= twake ||
                       signal::interrupted();
            }, TRANSFER_TIMEOUT_NS);
            s->wait.lock().lock();
            if (signal::interrupted()) {
                error = -err::INTR;
                break;
            }
            if (!ok && state(s) != STATE_PAUSED) {
                klog::warn("sound: %s %s timed out (DMA or interrupt trouble?)", s->pcm->name,
                           write ? "write" : "read");
                error = -err::IO;
                break;
            }
            continue;
        }
        u64 bs = s->cfg.buffer_size;
        u64 ofs = s->control->appl_ptr % bs;
        u64 n = min(frames - done, min(a, bs - ofs));
        // Copy with the lock dropped: the user buffer may need faulting in.
        // The region is ours (the hardware is on the other side of the
        // ring), so nothing else touches it meanwhile.
        s->wait.lock().unlock();
        long r = copy_frames(s, write, user_buf, chan_ptrs, done, ofs, n);
        s->wait.lock().lock();
        if (r < 0) {
            error = r;
            break;
        }
        if (!transfer_state_ok(s)) { // stopped while we copied
            error = transfer_state_error(s);
            break;
        }
        u64 appl = s->control->appl_ptr + n;
        if (appl >= s->boundary)
            appl -= s->boundary;
        s->control->appl_ptr = appl;
        done += n;
        if (write && state(s) == STATE_PREPARED && static_cast<u64>(hw_avail(s)) >= s->start_threshold) {
            if (long sr = start_locked(s); sr < 0) {
                error = sr;
                break;
            }
        }
    }
    s->wait.lock().unlock();
    if (done)
        wake(s);
    return done ? static_cast<i64>(done) : error;
}

// ---- Configuration ----

void fill_status(Substream* s, PcmStatus* st) {
    // `st` may carry the requested audio timestamp type (STATUS_EXT); keep it.
    u32 requested = st->audio_tstamp_data;
    memset(st, 0, sizeof(*st));
    st->state = state(s);
    st->trigger_tstamp = s->trigger_tstamp;
    st->tstamp = now_ts(s);
    st->appl_ptr = s->control->appl_ptr;
    st->hw_ptr = s->status->hw_ptr;
    int sv = state(s);
    if (sv == STATE_RUNNING || sv == STATE_DRAINING || sv == STATE_PAUSED || sv == STATE_PREPARED)
        st->delay = is_playback(s) ? hw_avail(s) : static_cast<i64>(avail(s));
    st->avail = s->configured ? avail(s) : 0;
    st->avail_max = max(s->avail_max, st->avail);
    s->avail_max = 0;
    st->suspended_state = s->status->suspended_state;
    // Audio timestamp: how much audio the hardware has processed, as time.
    st->audio_tstamp_data = requested & 0x1;
    if (s->cfg.rate)
        st->audio_tstamp = to_timespec(s->status->hw_ptr * 1'000'000'000ull / s->cfg.rate);
    st->driver_tstamp = st->tstamp;
}

u64 interval_value(const HwParams& p, int k) {
    const Interval& i = p.intervals[k - PARAM_FIRST_INTERVAL];
    return (i.openmin && !i.openmax) ? i.max : i.min;
}

int mask_value(const HwParams& p, int k) {
    const Mask& m = p.masks[k - PARAM_FIRST_MASK];
    for (int b = 0; b < 256; b++)
        if (m.bits[b / 32] & (1u << (b % 32)))
            return b;
    return -1;
}

long do_hw_params(Substream* s, HwParams* p) {
    LockGuard cfg(s->config_lock);
    {
        LockGuard guard(s->wait.lock());
        int st = state(s);
        if (st != STATE_OPEN && st != STATE_SETUP && st != STATE_PREPARED && st != STATE_XRUN)
            return -snderr::BADFD;
    }
    long r = internal::choose(s, p);
    if (r < 0)
        return r;
    PcmConfig c;
    c.access = mask_value(*p, PARAM_ACCESS);
    c.format = mask_value(*p, PARAM_FORMAT);
    c.channels = static_cast<u32>(interval_value(*p, PARAM_CHANNELS));
    c.rate = static_cast<u32>(interval_value(*p, PARAM_RATE));
    c.sample_bits = static_cast<u32>(format_physical_width(c.format));
    c.frame_bytes = c.sample_bits / 8 * c.channels;
    c.period_size = interval_value(*p, PARAM_PERIOD_SIZE);
    c.periods = interval_value(*p, PARAM_PERIODS);
    c.buffer_size = interval_value(*p, PARAM_BUFFER_SIZE);
    if (c.access < 0 || c.format < 0 || !c.channels || !c.rate || !c.frame_bytes || !c.period_size ||
        c.buffer_size < c.period_size || c.buffer_size * c.frame_bytes > s->dma_bytes)
        return -err::INVAL;
    if (s->configured && s->ops->hw_free)
        s->ops->hw_free(s);
    // Linux: boundary is the largest buffer_size * 2^n that fits in a long.
    u64 boundary = c.buffer_size;
    while (boundary * 2 <= LONG_MAX_U - c.buffer_size)
        boundary *= 2;

    LockGuard guard(s->wait.lock());
    s->cfg = c;
    s->boundary = boundary;
    // Default software parameters (what libasound's snd_pcm_sw_params_default sets too).
    s->start_threshold = 1;
    s->stop_threshold = c.buffer_size;
    s->silence_threshold = s->silence_size = 0;
    s->control->avail_min = c.period_size;
    s->control->appl_ptr = 0;
    s->status->hw_ptr = 0;
    s->hw_ptr_base = s->last_pos = 0;
    s->configured = true;
    s->status->state = STATE_SETUP;
    return 0;
}

long do_hw_free(Substream* s) {
    LockGuard cfg(s->config_lock);
    {
        LockGuard guard(s->wait.lock());
        int st = state(s);
        if (st == STATE_RUNNING || st == STATE_DRAINING || st == STATE_PAUSED)
            return -snderr::BADFD;
    }
    if (s->configured && s->ops->hw_free)
        s->ops->hw_free(s);
    LockGuard guard(s->wait.lock());
    s->configured = false;
    s->status->state = STATE_OPEN;
    return 0;
}

long do_sw_params(Substream* s, SwParams* p) {
    LockGuard cfg(s->config_lock);
    if (!s->configured)
        return -snderr::BADFD;
    if (p->tstamp_mode < 0 || p->tstamp_mode > 1)
        return -err::INVAL;
    if (p->proto >= protocol_version(2, 0, 12) && p->tstamp_type > 2)
        return -err::INVAL;
    if (p->avail_min == 0)
        return -err::INVAL;
    if (p->silence_size >= s->boundary) {
        if (p->silence_threshold != 0)
            return -err::INVAL;
    } else if (p->silence_size > p->silence_threshold || p->silence_threshold > s->cfg.buffer_size) {
        return -err::INVAL;
    }
    LockGuard guard(s->wait.lock());
    s->tstamp_mode = p->tstamp_mode;
    if (p->proto >= protocol_version(2, 0, 12))
        s->tstamp_type = static_cast<int>(p->tstamp_type);
    s->control->avail_min = p->avail_min;
    s->start_threshold = p->start_threshold;
    s->stop_threshold = p->stop_threshold;
    s->silence_threshold = p->silence_threshold;
    s->silence_size = p->silence_size;
    p->boundary = s->boundary;
    return 0;
}

long do_prepare(Substream* s) {
    LockGuard cfg(s->config_lock);
    {
        LockGuard guard(s->wait.lock());
        if (!s->configured || state(s) == STATE_OPEN || state(s) == STATE_DISCONNECTED)
            return -snderr::BADFD;
        if (state(s) == STATE_PAUSED || state(s) == STATE_SUSPENDED)
            stop_locked(s, STATE_SETUP);
        if (state(s) == STATE_RUNNING || state(s) == STATE_DRAINING)
            return -err::BUSY;
    }
    long r = s->ops->prepare(s);
    if (r < 0)
        return r;
    LockGuard guard(s->wait.lock());
    s->status->hw_ptr = 0;
    s->control->appl_ptr = 0;
    s->hw_ptr_base = s->last_pos = 0;
    s->avail_max = 0;
    if (is_playback(s))
        memset(s->dma_area, 0, s->cfg.buffer_size * s->cfg.frame_bytes); // start from silence
    s->status->state = STATE_PREPARED;
    return 0;
}

long do_drop(Substream* s) {
    {
        LockGuard guard(s->wait.lock());
        int st = state(s);
        if (st == STATE_OPEN || st == STATE_DISCONNECTED || !s->configured)
            return -snderr::BADFD;
        if (st != STATE_SETUP)
            stop_locked(s, STATE_SETUP);
    }
    wake(s);
    return 0;
}

long do_drain(Substream* s, vfs::File* f) {
    s->wait.lock().lock();
    int st = state(s);
    long r = 0;
    if (st == STATE_OPEN || st == STATE_DISCONNECTED || !s->configured) {
        r = -snderr::BADFD;
    } else if (st == STATE_SUSPENDED) {
        r = -snderr::STRPIPE;
    } else if (is_playback(s)) {
        if (st == STATE_PAUSED) { // Linux releases the pause and drains
            s->ops->trigger(s, TRIGGER_PAUSE_RELEASE);
            s->status->state = st = STATE_RUNNING;
            s->last_update_ns = clock::now_ns();
        }
        if (st == STATE_PREPARED) {
            if (hw_avail(s) > 0 && start_locked(s) == 0)
                st = STATE_RUNNING;
            else
                s->status->state = STATE_SETUP;
        } else if (st == STATE_XRUN) {
            s->status->state = STATE_SETUP;
        }
        if (st == STATE_RUNNING) {
            // Silence everything past the application's data so the tail
            // plays silence (not stale samples) until we notice the end.
            silence(s, s->control->appl_ptr, playback_avail(s));
            s->status->state = STATE_DRAINING;
        }
    } else { // capture: stop; what's already captured can still be read
        if (st == STATE_RUNNING || st == STATE_PAUSED) {
            update_locked(s);
            stop_locked(s, avail(s) ? STATE_DRAINING : STATE_SETUP);
        } else if (st == STATE_PREPARED || st == STATE_XRUN) {
            s->status->state = STATE_SETUP;
        }
    }
    bool draining = is_playback(s) && state(s) == STATE_DRAINING;
    s->wait.lock().unlock();
    if (r < 0 || !draining)
        return r;
    if (f->flags & vfs::O_NONBLOCK)
        return -err::AGAIN;
    s->wait.wait_until([&] {
        update_locked(s);
        return state(s) != STATE_DRAINING || signal::interrupted();
    });
    if (signal::interrupted()) {
        LockGuard guard(s->wait.lock());
        if (state(s) == STATE_DRAINING)
            return -err::INTR;
    }
    wake(s);
    return 0;
}

long do_pause(Substream* s, bool push) {
    LockGuard guard(s->wait.lock());
    if (!(s->hw.info & INFO_PAUSE))
        return -err::NOSYS;
    if (push) {
        if (state(s) != STATE_RUNNING)
            return -snderr::BADFD;
        update_locked(s);
        if (state(s) != STATE_RUNNING)
            return -err::PIPE;
        if (long r = s->ops->trigger(s, TRIGGER_PAUSE_PUSH); r < 0)
            return r;
        s->status->state = STATE_PAUSED;
    } else {
        if (state(s) != STATE_PAUSED)
            return -snderr::BADFD;
        if (long r = s->ops->trigger(s, TRIGGER_PAUSE_RELEASE); r < 0)
            return r;
        s->status->state = STATE_RUNNING;
        s->last_update_ns = clock::now_ns(); // the paused time isn't playback time
        internal::timer_kick();
    }
    s->trigger_tstamp = now_ts(s);
    return 0;
}

// REWIND (back = true) / FORWARD: move appl_ptr without transferring data.
long do_move(Substream* s, u64* frames, bool back) {
    LockGuard guard(s->wait.lock());
    switch (state(s)) {
    case STATE_PREPARED: case STATE_PAUSED: break;
    case STATE_RUNNING: case STATE_DRAINING: update_locked(s); break;
    case STATE_XRUN: return -err::PIPE;
    case STATE_SUSPENDED: return -snderr::STRPIPE;
    default: return -snderr::BADFD;
    }
    if (state(s) == STATE_XRUN)
        return -err::PIPE;
    i64 limit;
    if (back) // rewindable: what the hardware hasn't consumed yet
        limit = is_playback(s) ? hw_avail(s) : static_cast<i64>(s->cfg.buffer_size) - static_cast<i64>(avail(s));
    else      // forwardable: free space / unread data
        limit = static_cast<i64>(avail(s));
    u64 n = min(*frames, static_cast<u64>(max<i64>(limit, 0)));
    i64 appl = static_cast<i64>(s->control->appl_ptr) + (back ? -static_cast<i64>(n) : static_cast<i64>(n));
    if (appl < 0)
        appl += static_cast<i64>(s->boundary);
    else if (static_cast<u64>(appl) >= s->boundary)
        appl -= static_cast<i64>(s->boundary);
    s->control->appl_ptr = static_cast<u64>(appl);
    *frames = n;
    return 0;
}

long do_sync_ptr(Substream* s, SyncPtr* sp) {
    LockGuard guard(s->wait.lock());
    if (sp->flags & SYNC_PTR_HWSYNC)
        if (long r = hwsync_locked(s); r < 0)
            return r;
    if (!(sp->flags & SYNC_PTR_APPL)) {
        if (s->configured && sp->c.control.appl_ptr >= s->boundary)
            return -err::INVAL;
        s->control->appl_ptr = sp->c.control.appl_ptr;
    }
    if (!(sp->flags & SYNC_PTR_AVAIL_MIN))
        s->control->avail_min = sp->c.control.avail_min;
    memset(&sp->s, 0, sizeof(sp->s));
    memset(&sp->c, 0, sizeof(sp->c));
    sp->s.status = *s->status;
    sp->c.control = *s->control;
    return 0;
}

// Linked streams (LINK): start/stop/prepare together. Only pairs are kept
// (e.g. playback + capture of a full-duplex application).
struct LinkTable {
    Substream* a[8];
    Substream* b[8];
};
LinkTable g_links{};
IrqSpinlock g_link_lock;

Substream* linked(Substream* s) {
    LockGuard guard(g_link_lock);
    for (int i = 0; i < 8; i++) {
        if (g_links.a[i] == s) return g_links.b[i];
        if (g_links.b[i] == s) return g_links.a[i];
    }
    return nullptr;
}

void unlink_stream(Substream* s) {
    LockGuard guard(g_link_lock);
    for (int i = 0; i < 8; i++)
        if (g_links.a[i] == s || g_links.b[i] == s)
            g_links.a[i] = g_links.b[i] = nullptr;
}

long link_streams(Substream* s, Substream* other) {
    if (other == s || linked(s) || linked(other))
        return -snderr::ALREADY;
    LockGuard guard(g_link_lock);
    for (int i = 0; i < 8; i++) {
        if (!g_links.a[i]) {
            g_links.a[i] = s;
            g_links.b[i] = other;
            return 0;
        }
    }
    return -err::NOMEM;
}

// ---- The device node ----

class PcmNode;
PcmNode* g_nodes[32];
u32 g_node_count = 0;

class PcmNode : public vfs::Inode {
public:
    explicit PcmNode(Substream* s) : m_s(s) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0660;
        gid = vfs::GID_AUDIO;
        u32 minor = static_cast<u32>(s->card->number) * 32 + (is_playback(s) ? 16 : 24) + s->pcm->device;
        rdev = (116u << 8) | minor; // ALSA's static minor numbering
        gid = 29; // "audio"
        atime = mtime = ctime = vfs::now();
    }

    Substream* substream() const { return m_s; }
    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override {
        // One opener at a time, like a Linux PCM with a single subdevice.
        // open() can't fail here, so a second opener gets a file whose every
        // operation returns -EBUSY (libasound fails its first ioctl).
        LockGuard guard(m_s->wait.lock());
        if (m_s->open) {
            f->private_data = nullptr;
            return;
        }
        m_s->open = true;
        f->private_data = m_s;
    }

    void on_release(vfs::File* f) override {
        if (f->private_data != m_s)
            return;
        Substream* s = m_s;
        if (Substream* peer = linked(s); peer) {
            (void)peer;
            unlink_stream(s);
        }
        {
            LockGuard guard(s->wait.lock());
            if (state(s) != STATE_OPEN && state(s) != STATE_SETUP)
                stop_locked(s, STATE_SETUP);
        }
        do_hw_free(s);
        {
            LockGuard guard(s->wait.lock());
            s->open = false;
            s->tstamp_type = s->tstamp_mode = 0;
        }
        wake(s); // takes the wait queue's lock itself
    }

    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        if (f->private_data != m_s)
            return -err::BUSY;
        if (!m_s->configured)
            return -snderr::BADFD;
        u64 frames = count / m_s->cfg.frame_bytes;
        i64 r = transfer(m_s, f, true, reinterpret_cast<u64>(buf), nullptr, frames);
        return r < 0 ? r : static_cast<isize>(r * m_s->cfg.frame_bytes);
    }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        if (f->private_data != m_s)
            return -err::BUSY;
        if (!m_s->configured)
            return -snderr::BADFD;
        u64 frames = count / m_s->cfg.frame_bytes;
        i64 r = transfer(m_s, f, false, reinterpret_cast<u64>(buf), nullptr, frames);
        return r < 0 ? r : static_cast<isize>(r * m_s->cfg.frame_bytes);
    }

    u16 poll(vfs::File* f) override {
        if (f->private_data != m_s)
            return poll::POLLERR;
        Substream* s = m_s;
        u16 ok = is_playback(s) ? (poll::POLLOUT | poll::POLLWRNORM) : (poll::POLLIN | poll::POLLRDNORM);
        LockGuard guard(s->wait.lock());
        if (!s->configured)
            return ok | poll::POLLERR;
        update_locked(s);
        u64 a = avail(s);
        switch (state(s)) {
        case STATE_RUNNING: case STATE_PREPARED: case STATE_PAUSED:
            return a >= s->control->avail_min ? ok : 0;
        case STATE_DRAINING:
            if (is_playback(s))
                return 0;
            return a ? ok : (ok | poll::POLLERR);
        default:
            return ok | poll::POLLERR;
        }
    }

    long mmap_phys(u64 offset, u64* phys) override {
        Substream* s = m_s;
        if (offset == MMAP_OFFSET_STATUS_OLD || offset == MMAP_OFFSET_STATUS_NEW) {
            *phys = s->status_phys;
            return 0;
        }
        if (offset == MMAP_OFFSET_CONTROL_OLD || offset == MMAP_OFFSET_CONTROL_NEW) {
            *phys = s->control_phys;
            return 0;
        }
        if (!(s->hw.info & INFO_MMAP) || offset >= align_up<u64>(s->dma_bytes, 4096))
            return -err::INVAL;
        *phys = s->dma_phys + offset;
        return 0;
    }

    long ioctl_file(vfs::File* f, u64 req, u64 arg) override;

private:
    Substream* m_s;
};

Substream* substream_of_fd(int fd) {
    vfs::File* f = proc::fd_get(proc::current(), fd);
    if (!f)
        return nullptr;
    Substream* found = nullptr;
    for (u32 i = 0; i < g_node_count; i++)
        if (g_nodes[i] == f->inode && f->private_data == g_nodes[i]->substream())
            found = g_nodes[i]->substream();
    vfs::file_put(f);
    return found;
}

template <typename T>
long get_arg(T* out, u64 arg) { return uaccess::copy_from(out, reinterpret_cast<const void*>(arg), sizeof(T)); }
template <typename T>
long put_arg(u64 arg, const T& v) { return uaccess::copy_to(reinterpret_cast<void*>(arg), &v, sizeof(T)); }

long PcmNode::ioctl_file(vfs::File* f, u64 req, u64 arg) {
    Substream* s = m_s;
    if (ioc_type(req) != 'A')
        return -err::NOTTY;
    if (f->private_data != s)
        return -err::BUSY;
    u32 nr = ioc_nr(req);
    switch (nr) {
    case PCM_PVERSION:
        return put_arg<i32>(arg, PCM_VERSION);
    case PCM_INFO: {
        auto* info = new PcmInfo{};
        info->device = s->pcm->device;
        info->stream = s->stream;
        info->card = s->card->number;
        copy_name(info->id, s->pcm->id, sizeof(info->id));
        copy_name(info->name, s->pcm->name, sizeof(info->name));
        copy_name(info->subname, "subdevice #0", sizeof(info->subname));
        info->subdevices_count = 1;
        info->subdevices_avail = 0; // we are the one using it
        long r = put_arg(arg, *info);
        delete info;
        return r;
    }
    case PCM_TSTAMP: {
        i32 v;
        if (long r = get_arg(&v, arg); r < 0) return r;
        LockGuard guard(s->wait.lock());
        s->tstamp_mode = v ? 1 : 0;
        return 0;
    }
    case PCM_TTSTAMP: {
        i32 v;
        if (long r = get_arg(&v, arg); r < 0) return r;
        if (v < 0 || v > 2) return -err::INVAL;
        LockGuard guard(s->wait.lock());
        s->tstamp_type = v;
        return 0;
    }
    case PCM_USER_PVERSION:
        return 0; // libasound tells us its protocol version; nothing depends on it yet
    case PCM_HW_REFINE:
    case PCM_HW_PARAMS: {
        auto* p = new HwParams;
        long r = get_arg(p, arg);
        if (r == 0)
            r = nr == PCM_HW_REFINE ? internal::refine(s, p) : do_hw_params(s, p);
        if (r == 0)
            r = put_arg(arg, *p);
        delete p;
        return r;
    }
    case PCM_HW_FREE:
        return do_hw_free(s);
    case PCM_SW_PARAMS: {
        SwParams p;
        long r = get_arg(&p, arg);
        if (r == 0) r = do_sw_params(s, &p);
        if (r == 0) r = put_arg(arg, p);
        return r;
    }
    case PCM_STATUS:
    case PCM_STATUS_EXT: {
        PcmStatus st{};
        if (nr == PCM_STATUS_EXT)
            if (long r = get_arg(&st, arg); r < 0) return r;
        {
            LockGuard guard(s->wait.lock());
            update_locked(s);
            fill_status(s, &st);
        }
        return put_arg(arg, st);
    }
    case PCM_DELAY: {
        i64 d;
        {
            LockGuard guard(s->wait.lock());
            if (long r = hwsync_locked(s); r < 0) return r;
            d = is_playback(s) ? hw_avail(s) : static_cast<i64>(avail(s));
        }
        return put_arg(arg, d);
    }
    case PCM_HWSYNC: {
        LockGuard guard(s->wait.lock());
        return hwsync_locked(s);
    }
    case PCM_SYNC_PTR: {
        SyncPtr sp;
        long r = get_arg(&sp, arg);
        if (r == 0) r = do_sync_ptr(s, &sp);
        if (r == 0) r = put_arg(arg, sp);
        return r;
    }
    case PCM_CHANNEL_INFO: {
        ChannelInfo ci;
        if (long r = get_arg(&ci, arg); r < 0) return r;
        if (!s->configured) return -snderr::BADFD;
        if (ci.channel >= s->cfg.channels) return -err::INVAL;
        ci.offset = 0;
        ci.first = ci.channel * s->cfg.sample_bits;
        ci.step = s->cfg.frame_bytes * 8;
        return put_arg(arg, ci);
    }
    case PCM_PREPARE: {
        long r = do_prepare(s);
        if (Substream* peer = linked(s); r == 0 && peer)
            do_prepare(peer);
        return r;
    }
    case PCM_RESET: {
        LockGuard guard(s->wait.lock());
        int st = state(s);
        if (st != STATE_RUNNING && st != STATE_PREPARED && st != STATE_PAUSED)
            return st == STATE_XRUN ? -err::PIPE : -snderr::BADFD;
        update_locked(s);
        s->control->appl_ptr = s->status->hw_ptr;
        s->avail_max = 0;
        return 0;
    }
    case PCM_START: {
        long r;
        {
            LockGuard guard(s->wait.lock());
            r = start_locked(s);
        }
        if (Substream* peer = linked(s); r == 0 && peer) {
            LockGuard guard(peer->wait.lock());
            start_locked(peer);
        }
        return r;
    }
    case PCM_DROP: {
        long r = do_drop(s);
        if (Substream* peer = linked(s); peer)
            do_drop(peer);
        return r;
    }
    case PCM_DRAIN:
        return do_drain(s, f);
    case PCM_PAUSE: // the argument is the value itself, not a pointer
        return do_pause(s, arg != 0);
    case PCM_REWIND:
    case PCM_FORWARD: {
        u64 frames;
        if (long r = get_arg(&frames, arg); r < 0) return r;
        if (long r = do_move(s, &frames, nr == PCM_REWIND); r < 0) return r;
        return put_arg(arg, frames);
    }
    case PCM_RESUME:
        return -err::NOSYS; // we never suspend
    case PCM_XRUN: {
        LockGuard guard(s->wait.lock());
        switch (state(s)) {
        case STATE_XRUN: return 0;
        case STATE_RUNNING: case STATE_DRAINING: case STATE_PAUSED: xrun_locked(s); return 0;
        default: return -snderr::BADFD;
        }
    }
    case PCM_WRITEI_FRAMES:
    case PCM_READI_FRAMES: {
        Xferi x;
        if (long r = get_arg(&x, arg); r < 0) return r;
        i64 r = transfer(s, f, nr == PCM_WRITEI_FRAMES, x.buf, nullptr, x.frames);
        x.result = r;
        if (long pr = put_arg(arg, x); pr < 0) return pr;
        return r < 0 ? r : 0;
    }
    case PCM_WRITEN_FRAMES:
    case PCM_READN_FRAMES: {
        Xfern x;
        if (long r = get_arg(&x, arg); r < 0) return r;
        if (!s->configured) return -snderr::BADFD;
        u64 ptrs[32];
        if (s->cfg.channels > 32) return -err::INVAL;
        if (long r = uaccess::copy_from(ptrs, reinterpret_cast<const void*>(x.bufs), s->cfg.channels * sizeof(u64)); r < 0)
            return r;
        i64 r = transfer(s, f, nr == PCM_WRITEN_FRAMES, 0, ptrs, x.frames);
        x.result = r;
        if (long pr = put_arg(arg, x); pr < 0) return pr;
        return r < 0 ? r : 0;
    }
    case PCM_LINK: { // the argument is the other stream's file descriptor
        Substream* other = substream_of_fd(static_cast<int>(arg));
        if (!other) return -err::BADF;
        return link_streams(s, other);
    }
    case PCM_UNLINK:
        if (!linked(s)) return -snderr::ALREADY;
        unlink_stream(s);
        return 0;
    default:
        return -err::NOTTY;
    }
}

} // namespace

namespace internal {

void register_pcm_node(Substream* s, const char* path) {
    auto* node = new PcmNode(s);
    if (g_node_count < array_size(g_nodes))
        g_nodes[g_node_count++] = node;
    vfs::add_node(path, node);
}

// Poll every running stream's position (the safety net behind period
// interrupts). Returns true if any stream is running.
bool update_running(Substream* s) {
    if (!s)
        return false;
    bool changed, running;
    {
        LockGuard guard(s->wait.lock());
        changed = update_locked(s);
        running = state(s) == STATE_RUNNING || state(s) == STATE_DRAINING;
    }
    if (changed)
        wake(s);
    return running;
}

} // namespace internal

void period_elapsed(Substream* s) { internal::update_running(s); }

} // namespace sound
