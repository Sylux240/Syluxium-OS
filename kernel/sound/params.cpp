// PCM hardware-parameter refinement (HW_REFINE / HW_PARAMS).
//
// libasound negotiates a configuration by repeatedly sending a "space" of
// candidate parameters (bit masks for access/format/subformat, intervals for
// everything else) and asking the kernel to shrink it to what the hardware
// can actually do. The dependencies between parameters (bytes = frames *
// frame_bits / 8, time = frames * 10^6 / rate, buffer = period * periods,
// ...) are expressed as rules that are applied until nothing changes. This
// is a faithful port of Linux's interval arithmetic (sound/core/pcm_lib.c),
// including its open/closed bound semantics, because libasound's own
// refinement code (pcm_params.c) is the same algorithm and expects the
// kernel's answers to be consistent with it.

#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <sound/sound.hpp>

namespace sound {

int format_physical_width(int f) {
    using namespace abi;
    switch (f) {
    case FORMAT_S8: case FORMAT_U8: return 8;
    case FORMAT_S16_LE: case FORMAT_S16_BE: case FORMAT_U16_LE: case FORMAT_U16_BE: return 16;
    case FORMAT_S24_LE: case FORMAT_S24_BE: case FORMAT_U24_LE: case FORMAT_U24_BE:
    case FORMAT_S32_LE: case FORMAT_S32_BE: case FORMAT_U32_LE: case FORMAT_U32_BE:
    case FORMAT_FLOAT_LE: case FORMAT_FLOAT_BE: case FORMAT_S20_LE: return 32;
    case FORMAT_FLOAT64_LE: case FORMAT_FLOAT64_BE: return 64;
    case FORMAT_S24_3LE: case FORMAT_S20_3LE: return 24;
    default: return -1;
    }
}

int format_width(int f) {
    using namespace abi;
    switch (f) {
    case FORMAT_S24_LE: case FORMAT_S24_BE: case FORMAT_U24_LE: case FORMAT_U24_BE: case FORMAT_S24_3LE: return 24;
    case FORMAT_S20_LE: case FORMAT_S20_3LE: return 20;
    default: return format_physical_width(f);
    }
}

const char* format_name(int f) {
    using namespace abi;
    switch (f) {
    case FORMAT_S8: return "S8";
    case FORMAT_U8: return "U8";
    case FORMAT_S16_LE: return "S16_LE";
    case FORMAT_S24_LE: return "S24_LE";
    case FORMAT_S32_LE: return "S32_LE";
    case FORMAT_FLOAT_LE: return "FLOAT_LE";
    case FORMAT_S24_3LE: return "S24_3LE";
    default: return "?";
    }
}

namespace internal {

namespace {

using abi::HwParams;
using abi::Interval;
using abi::Mask;

constexpr u32 UMAX = 0xFFFFFFFFu;

Interval& iv(HwParams* p, int k) { return p->intervals[k - abi::PARAM_FIRST_INTERVAL]; }
Mask& mk(HwParams* p, int k) { return p->masks[k - abi::PARAM_FIRST_MASK]; }
bool is_mask(int k) { return k <= abi::PARAM_LAST_MASK; }

// ---- Intervals ----

bool iv_checkempty(const Interval& i) { return i.min > i.max || (i.min == i.max && (i.openmin || i.openmax)); }
bool iv_single(const Interval& i) { return i.min == i.max || (i.min + 1 == i.max && (i.openmin || i.openmax)); }
u32 iv_value(const Interval& i) { return (i.openmin && !i.openmax) ? i.max : i.min; }
void iv_none(Interval& i) { i.empty = 1; }

Interval iv_make(u32 lo, u32 hi, bool integer) {
    Interval i{};
    i.min = lo;
    i.max = hi;
    i.integer = integer;
    return i;
}

bool iv_test(const Interval& i, u32 v) {
    return !(i.min > v || (i.min == v && i.openmin) || i.max < v || (i.max == v && i.openmax));
}

// Intersect `i` with `v`. Returns 1 if `i` changed, 0 if not, -EINVAL if empty.
int iv_refine(Interval& i, const Interval& v) {
    if (i.empty)
        return -err::INVAL;
    int changed = 0;
    if (i.min < v.min) {
        i.min = v.min;
        i.openmin = v.openmin;
        changed = 1;
    } else if (i.min == v.min && !i.openmin && v.openmin) {
        i.openmin = 1;
        changed = 1;
    }
    if (i.max > v.max) {
        i.max = v.max;
        i.openmax = v.openmax;
        changed = 1;
    } else if (i.max == v.max && !i.openmax && v.openmax) {
        i.openmax = 1;
        changed = 1;
    }
    if (!i.integer && v.integer) {
        i.integer = 1;
        changed = 1;
    }
    if (i.integer) {
        if (i.openmin) {
            i.min++;
            i.openmin = 0;
        }
        if (i.openmax) {
            i.max--;
            i.openmax = 0;
        }
    } else if (!i.openmin && !i.openmax && i.min == i.max) {
        i.integer = 1;
    }
    if (iv_checkempty(i)) {
        iv_none(i);
        return -err::INVAL;
    }
    return changed;
}

int iv_refine_first(Interval& i) {
    if (i.empty)
        return -err::INVAL;
    if (iv_single(i))
        return 0;
    u32 last_max = i.max;
    i.max = i.min;
    if (i.openmin)
        i.max++;
    i.openmax = i.openmax && i.max >= last_max; // keep max excluded only if it was before
    return 1;
}

int iv_refine_last(Interval& i) {
    if (i.empty)
        return -err::INVAL;
    if (iv_single(i))
        return 0;
    u32 last_min = i.min;
    i.min = i.max;
    if (i.openmax)
        i.min--;
    i.openmin = i.openmin && i.min <= last_min;
    return 1;
}

u32 mul32(u32 a, u32 b) {
    u64 r = static_cast<u64>(a) * b;
    return r > UMAX ? UMAX : static_cast<u32>(r);
}

u32 div32(u32 a, u32 b, u32* rem) {
    if (b == 0) {
        *rem = 0;
        return UMAX;
    }
    *rem = a % b;
    return a / b;
}

u32 muldiv32(u32 a, u32 b, u32 c, u32* rem) {
    u64 n = static_cast<u64>(a) * b;
    if (c == 0 || n >= (static_cast<u64>(c) << 32)) {
        *rem = 0;
        return UMAX;
    }
    *rem = static_cast<u32>(n % c);
    return static_cast<u32>(n / c);
}

// c = a * b
void iv_mul(const Interval& a, const Interval& b, Interval& c) {
    c = {};
    if (a.empty || b.empty) {
        iv_none(c);
        return;
    }
    c.min = mul32(a.min, b.min);
    c.openmin = a.openmin || b.openmin;
    c.max = mul32(a.max, b.max);
    c.openmax = a.openmax || b.openmax;
    c.integer = a.integer && b.integer;
}

// c = a / b
void iv_div(const Interval& a, const Interval& b, Interval& c) {
    c = {};
    if (a.empty || b.empty) {
        iv_none(c);
        return;
    }
    u32 r;
    c.min = div32(a.min, b.max, &r);
    c.openmin = r || a.openmin || b.openmax;
    if (b.min > 0) {
        c.max = div32(a.max, b.min, &r);
        if (r) {
            c.max++;
            c.openmax = 1;
        } else {
            c.openmax = a.openmax || b.openmin;
        }
    } else {
        c.max = UMAX;
        c.openmax = 0;
    }
    c.integer = 0;
}

// c = a * b / k
void iv_muldivk(const Interval& a, const Interval& b, u32 k, Interval& c) {
    c = {};
    if (a.empty || b.empty) {
        iv_none(c);
        return;
    }
    u32 r;
    c.min = muldiv32(a.min, b.min, k, &r);
    c.openmin = r || a.openmin || b.openmin;
    c.max = muldiv32(a.max, b.max, k, &r);
    if (r) {
        c.max++;
        c.openmax = 1;
    } else {
        c.openmax = a.openmax || b.openmax;
    }
    c.integer = 0;
}

// c = a * k / b
void iv_mulkdiv(const Interval& a, u32 k, const Interval& b, Interval& c) {
    c = {};
    if (a.empty || b.empty) {
        iv_none(c);
        return;
    }
    u32 r;
    c.min = muldiv32(a.min, k, b.max, &r);
    c.openmin = r || a.openmin || b.openmax;
    if (b.min > 0) {
        c.max = muldiv32(a.max, k, b.min, &r);
        if (r) {
            c.max++;
            c.openmax = 1;
        } else {
            c.openmax = a.openmax || b.openmin;
        }
    } else {
        c.max = UMAX;
        c.openmax = 0;
    }
    c.integer = 0;
}

// Restrict to the values in `list` that the interval still allows.
int iv_list(Interval& i, const u32* list, u32 count) {
    if (!count) {
        iv_none(i);
        return -err::INVAL;
    }
    Interval range{};
    range.min = UMAX;
    range.max = 0;
    for (u32 k = 0; k < count; k++) {
        if (!iv_test(i, list[k]))
            continue;
        range.min = min(range.min, list[k]);
        range.max = max(range.max, list[k]);
    }
    return iv_refine(i, range);
}

// Round the bounds inwards to multiples of `step`.
int iv_step(Interval& i, u32 step) {
    if (i.empty)
        return -err::INVAL;
    int changed = 0;
    u32 n = i.min % step;
    if (n != 0 || i.openmin) {
        i.min += step - n;
        i.openmin = 0;
        changed = 1;
    }
    n = i.max % step;
    if (n != 0 || i.openmax) {
        i.max -= n;
        i.openmax = 0;
        changed = 1;
    }
    if (iv_checkempty(i)) {
        iv_none(i);
        return -err::INVAL;
    }
    return changed;
}

// ---- Masks ----

bool mask_empty(const Mask& m) {
    for (u32 w : m.bits)
        if (w)
            return false;
    return true;
}

bool mask_test(const Mask& m, u32 bit) { return m.bits[bit / 32] & (1u << (bit % 32)); }

int mask_refine(Mask& m, const Mask& v) {
    int changed = 0;
    for (int k = 0; k < 8; k++) {
        u32 old = m.bits[k];
        m.bits[k] &= v.bits[k];
        if (m.bits[k] != old)
            changed = 1;
    }
    return mask_empty(m) ? -err::INVAL : changed;
}

int mask_refine_first(Mask& m) {
    if (mask_empty(m))
        return -err::INVAL;
    for (u32 b = 0; b < 256; b++) {
        if (mask_test(m, b)) {
            Mask single{};
            single.bits[b / 32] = 1u << (b % 32);
            bool changed = memcmp(&m, &single, sizeof(Mask)) != 0;
            m = single;
            return changed;
        }
    }
    return 0;
}

Mask mask_of(u64 bits) {
    Mask m{};
    m.bits[0] = static_cast<u32>(bits);
    m.bits[1] = static_cast<u32>(bits >> 32);
    return m;
}

// ---- Rules ----

enum RuleKind : u8 { MUL, DIV, MULDIVK, MULKDIV, FORMAT_BY_BITS, BITS_BY_FORMAT, RATE_LIST, STEP };

struct Rule {
    RuleKind kind;
    int var, a, b;
    u32 k;
};

using namespace abi;

// Linux's snd_pcm_hw_constraints_init() rule set.
constexpr Rule RULES[] = {
    {FORMAT_BY_BITS, PARAM_FORMAT, PARAM_SAMPLE_BITS, 0, 0},
    {BITS_BY_FORMAT, PARAM_SAMPLE_BITS, PARAM_FORMAT, 0, 0},
    {DIV, PARAM_SAMPLE_BITS, PARAM_FRAME_BITS, PARAM_CHANNELS, 0},
    {MUL, PARAM_FRAME_BITS, PARAM_SAMPLE_BITS, PARAM_CHANNELS, 0},
    {MULKDIV, PARAM_FRAME_BITS, PARAM_PERIOD_BYTES, PARAM_PERIOD_SIZE, 8},
    {MULKDIV, PARAM_FRAME_BITS, PARAM_BUFFER_BYTES, PARAM_BUFFER_SIZE, 8},
    {DIV, PARAM_CHANNELS, PARAM_FRAME_BITS, PARAM_SAMPLE_BITS, 0},
    {MULKDIV, PARAM_RATE, PARAM_PERIOD_SIZE, PARAM_PERIOD_TIME, 1000000},
    {MULKDIV, PARAM_RATE, PARAM_BUFFER_SIZE, PARAM_BUFFER_TIME, 1000000},
    {DIV, PARAM_PERIODS, PARAM_BUFFER_SIZE, PARAM_PERIOD_SIZE, 0},
    {DIV, PARAM_PERIOD_SIZE, PARAM_BUFFER_SIZE, PARAM_PERIODS, 0},
    {MULKDIV, PARAM_PERIOD_SIZE, PARAM_PERIOD_BYTES, PARAM_FRAME_BITS, 8},
    {MULDIVK, PARAM_PERIOD_SIZE, PARAM_PERIOD_TIME, PARAM_RATE, 1000000},
    {MUL, PARAM_BUFFER_SIZE, PARAM_PERIOD_SIZE, PARAM_PERIODS, 0},
    {MULKDIV, PARAM_BUFFER_SIZE, PARAM_BUFFER_BYTES, PARAM_FRAME_BITS, 8},
    {MULDIVK, PARAM_BUFFER_SIZE, PARAM_BUFFER_TIME, PARAM_RATE, 1000000},
    {MULDIVK, PARAM_PERIOD_BYTES, PARAM_PERIOD_SIZE, PARAM_FRAME_BITS, 8},
    {MULDIVK, PARAM_BUFFER_BYTES, PARAM_BUFFER_SIZE, PARAM_FRAME_BITS, 8},
    {MULKDIV, PARAM_PERIOD_TIME, PARAM_PERIOD_SIZE, PARAM_RATE, 1000000},
    {MULKDIV, PARAM_BUFFER_TIME, PARAM_BUFFER_SIZE, PARAM_RATE, 1000000},
    // Hardware-specific (snd_pcm_hw_constraints_complete + driver constraints).
    {RATE_LIST, PARAM_RATE, 0, 0, 0},
    {STEP, PARAM_PERIOD_BYTES, 0, 0, 0},
    {STEP, PARAM_BUFFER_BYTES, 0, 0, 0},
};

int apply_rule(const Rule& r, const Substream* s, HwParams* p) {
    Interval t;
    switch (r.kind) {
    case MUL:
        iv_mul(iv(p, r.a), iv(p, r.b), t);
        return iv_refine(iv(p, r.var), t);
    case DIV:
        iv_div(iv(p, r.a), iv(p, r.b), t);
        return iv_refine(iv(p, r.var), t);
    case MULDIVK:
        iv_muldivk(iv(p, r.a), iv(p, r.b), r.k, t);
        return iv_refine(iv(p, r.var), t);
    case MULKDIV:
        iv_mulkdiv(iv(p, r.a), r.k, iv(p, r.b), t);
        return iv_refine(iv(p, r.var), t);
    case FORMAT_BY_BITS: { // drop formats whose physical width the sample-bits interval excludes
        Mask m = mk(p, PARAM_FORMAT);
        const Interval& bits = iv(p, PARAM_SAMPLE_BITS);
        for (int f = 0; f <= FORMAT_LAST; f++) {
            if (!mask_test(m, f))
                continue;
            int w = format_physical_width(f);
            if (w <= 0)
                continue;
            if (static_cast<u32>(w) < bits.min || static_cast<u32>(w) > bits.max)
                m.bits[f / 32] &= ~(1u << (f % 32));
        }
        return mask_refine(mk(p, PARAM_FORMAT), m);
    }
    case BITS_BY_FORMAT: { // sample bits = range of the remaining formats' widths
        t = iv_make(UMAX, 0, true);
        const Mask& m = mk(p, PARAM_FORMAT);
        for (int f = 0; f <= FORMAT_LAST; f++) {
            if (!mask_test(m, f))
                continue;
            int w = format_physical_width(f);
            if (w <= 0)
                continue;
            t.min = min(t.min, static_cast<u32>(w));
            t.max = max(t.max, static_cast<u32>(w));
        }
        return iv_refine(iv(p, PARAM_SAMPLE_BITS), t);
    }
    case RATE_LIST:
        return iv_list(iv(p, PARAM_RATE), s->hw.rates, s->hw.rate_count);
    case STEP:
        return s->hw.byte_align > 1 ? iv_step(iv(p, r.var), s->hw.byte_align) : 0;
    }
    return 0;
}

// The hardware's fixed limits, applied before the rules.
int apply_hw_limits(const Substream* s, HwParams* p) {
    const PcmHardware& hw = s->hw;
    u64 access = 0;
    if (hw.info & INFO_INTERLEAVED) {
        // Non-interleaved read/write is offered too: the core (de)interleaves while copying.
        access |= (1u << ACCESS_RW_INTERLEAVED) | (1u << ACCESS_RW_NONINTERLEAVED);
        if (hw.info & INFO_MMAP)
            access |= 1u << ACCESS_MMAP_INTERLEAVED;
    }
    int r;
    if ((r = mask_refine(mk(p, PARAM_ACCESS), mask_of(access))) < 0) return r;
    if ((r = mask_refine(mk(p, PARAM_FORMAT), mask_of(hw.formats))) < 0) return r;
    if ((r = mask_refine(mk(p, PARAM_SUBFORMAT), mask_of(1u << SUBFORMAT_STD))) < 0) return r;

    u32 buffer_max = hw.buffer_bytes_max ? min(hw.buffer_bytes_max, s->dma_bytes) : s->dma_bytes;
    u32 period_max = hw.period_bytes_max ? min(hw.period_bytes_max, buffer_max) : buffer_max;
    struct { int param; Interval limit; } limits[] = {
        {PARAM_CHANNELS, iv_make(hw.channels_min, hw.channels_max, true)},
        {PARAM_RATE, iv_make(hw.rate_count ? hw.rates[0] : 1, hw.rate_count ? hw.rates[hw.rate_count - 1] : UMAX, false)},
        {PARAM_PERIOD_BYTES, iv_make(hw.period_bytes_min, period_max, false)},
        {PARAM_PERIODS, iv_make(hw.periods_min, hw.periods_max, true)},
        {PARAM_BUFFER_BYTES, iv_make(hw.period_bytes_min, buffer_max, true)},
        {PARAM_PERIOD_SIZE, iv_make(0, UMAX, true)},
        {PARAM_BUFFER_SIZE, iv_make(0, UMAX, true)},
        {PARAM_SAMPLE_BITS, iv_make(0, UMAX, true)},
        {PARAM_FRAME_BITS, iv_make(0, UMAX, true)},
    };
    for (auto& l : limits)
        if ((r = iv_refine(iv(p, l.param), l.limit)) < 0)
            return r;
    return 0;
}

int run_rules(const Substream* s, HwParams* p) {
    // Every rule only ever narrows its parameter, so iterating to a fixed
    // point terminates; the bound is a safety net against pathological input.
    for (int round = 0; round < 1000; round++) {
        bool changed = false;
        for (const Rule& r : RULES) {
            int c = apply_rule(r, s, p);
            if (c < 0)
                return c;
            changed |= c > 0;
        }
        if (!changed)
            return 0;
    }
    return 0;
}

void fill_info(const Substream* s, HwParams* p) {
    const Interval& bits = iv(p, PARAM_SAMPLE_BITS);
    if (iv_single(bits)) {
        p->msbits = iv_value(bits);
        // 32-bit containers often carry fewer significant bits (HDA: 24).
        const Mask& fm = mk(p, PARAM_FORMAT);
        if (s->hw.msbits && iv_value(bits) == 32 && mask_test(fm, FORMAT_S32_LE))
            p->msbits = s->hw.msbits;
    }
    const Interval& rate = iv(p, PARAM_RATE);
    if (iv_single(rate)) {
        p->rate_num = iv_value(rate);
        p->rate_den = 1;
    }
    p->fifo_size = 0;
    p->info = s->hw.info;
}

long refine_internal(const Substream* s, HwParams* p) {
    for (const Interval& i : p->intervals)
        if (i.empty)
            return -err::INVAL;
    HwParams before = *p;
    p->info = 0;
    p->msbits = 0;
    p->rate_num = p->rate_den = 0;
    int r = apply_hw_limits(s, p);
    if (r >= 0)
        r = run_rules(s, p);
    if (r < 0)
        return r;
    // Report which parameters changed.
    p->cmask = 0;
    for (int k = PARAM_FIRST_MASK; k <= PARAM_LAST_MASK; k++)
        if (memcmp(&before.masks[k], &p->masks[k], sizeof(Mask)) != 0)
            p->cmask |= 1u << k;
    for (int k = PARAM_FIRST_INTERVAL; k <= PARAM_LAST_INTERVAL; k++)
        if (memcmp(&before.intervals[k - PARAM_FIRST_INTERVAL], &p->intervals[k - PARAM_FIRST_INTERVAL],
                   sizeof(Interval)) != 0)
            p->cmask |= 1u << k;
    fill_info(s, p);
    p->rmask = 0;
    return 0;
}

} // namespace

long refine(const Substream* s, abi::HwParams* params) { return refine_internal(s, params); }

long choose(const Substream* s, abi::HwParams* p) {
    long r = refine_internal(s, p);
    if (r < 0)
        return r;
    u32 cmask = p->cmask;
    // Linux's snd_pcm_hw_params_choose() order (smallest value first, but the
    // largest buffer), plus period size/count so every interval ends up single.
    static constexpr int order[] = {PARAM_ACCESS, PARAM_FORMAT, PARAM_SUBFORMAT, PARAM_CHANNELS, PARAM_RATE,
                                    PARAM_PERIOD_TIME, PARAM_PERIOD_SIZE, PARAM_BUFFER_SIZE, PARAM_PERIODS,
                                    PARAM_TICK_TIME};
    for (int var : order) {
        int c;
        if (is_mask(var))
            c = mask_refine_first(mk(p, var));
        else if (var == PARAM_BUFFER_SIZE)
            c = iv_refine_last(iv(p, var));
        else
            c = iv_refine_first(iv(p, var));
        if (c < 0)
            return c;
        if (c > 0) {
            r = refine_internal(s, p);
            if (r < 0)
                return r;
            cmask |= p->cmask;
        }
    }
    p->cmask = cmask;
    return 0;
}

} // namespace internal

} // namespace sound
