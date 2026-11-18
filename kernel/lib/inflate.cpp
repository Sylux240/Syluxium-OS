#include <lib/errno.hpp>
#include <lib/inflate.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace inflate {

namespace {

// Canonical Huffman code with a 10-bit direct lookup table; longer codes
// are decoded bit by bit from the counts (they're rare).
constexpr int FAST_BITS = 10;

struct Huffman {
    u16 count[16];
    u16 symbol[320];
    u16 fast[1 << FAST_BITS]; // (length << 9) | symbol, 0 = use the slow path
};

struct State {
    const u8* in;
    usize in_len, in_pos;
    u64 bits;
    u32 nbits;
    u8* window; // 64 KiB ring; the last 32 KiB is history for back-references
    u32 out_pos; // total output, modulo 2^32
    Sink sink;
    void* ctx;
    bool failed;
};

void refill(State& s) {
    while (s.nbits <= 56) {
        u64 byte = s.in_pos < s.in_len ? s.in[s.in_pos] : 0;
        s.in_pos++;
        s.bits |= byte << s.nbits;
        s.nbits += 8;
    }
}

u32 getbits(State& s, u32 n) {
    if (n == 0)
        return 0;
    if (s.nbits < n)
        refill(s);
    u32 v = static_cast<u32>(s.bits & ((1ull << n) - 1));
    s.bits >>= n;
    s.nbits -= n;
    return v;
}

bool build(Huffman& h, const u8* lengths, int n) {
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++)
        h.count[lengths[i]]++;
    h.count[0] = 0;
    u16 offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = static_cast<u16>(offs[len] + h.count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h.symbol[offs[lengths[i]]++] = static_cast<u16>(i);
    // Fast table: walk the codes in canonical order.
    memset(h.fast, 0, sizeof(h.fast));
    u32 code = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        for (int k = 0; k < h.count[len]; k++, index++, code++) {
            if (len > FAST_BITS)
                continue;
            // Codes are sent MSB first; the bit reader is LSB first: reverse.
            u32 rev = 0;
            for (int b = 0; b < len; b++)
                rev |= ((code >> b) & 1) << (len - 1 - b);
            for (u32 fill = rev; fill < (1u << FAST_BITS); fill += 1u << len)
                h.fast[fill] = static_cast<u16>((len << 9) | h.symbol[index]);
        }
        code <<= 1;
    }
    return true;
}

int decode(State& s, const Huffman& h) {
    if (s.nbits < 16)
        refill(s);
    u16 f = h.fast[s.bits & ((1u << FAST_BITS) - 1)];
    if (f) {
        u32 len = f >> 9;
        s.bits >>= len;
        s.nbits -= len;
        return f & 0x1FF;
    }
    // Slow path (puff-style): one bit at a time.
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= static_cast<int>(getbits(s, 1));
        int count = h.count[len];
        if (code - count < first)
            return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

void put(State& s, u8 byte) {
    s.window[s.out_pos & 0xFFFF] = byte;
    s.out_pos++;
    if ((s.out_pos & 0x7FFF) == 0 && !s.failed) { // a 32 KiB half is complete
        u32 half = (s.out_pos - 0x8000) & 0xFFFF;
        if (!s.sink(s.ctx, s.window + half, 0x8000))
            s.failed = true;
    }
}

const u16 LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const u8 LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const u16 DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const u8 DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(State& s, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        if (s.failed || s.in_pos > s.in_len + 8)
            return false;
        int sym = decode(s, lit);
        if (sym < 0)
            return false;
        if (sym < 256) {
            put(s, static_cast<u8>(sym));
            continue;
        }
        if (sym == 256)
            return true;
        sym -= 257;
        if (sym >= 29)
            return false;
        u32 len = LEN_BASE[sym] + getbits(s, LEN_EXTRA[sym]);
        int ds = decode(s, dist);
        if (ds < 0 || ds >= 30)
            return false;
        u32 d = DIST_BASE[ds] + getbits(s, DIST_EXTRA[ds]);
        if (d > s.out_pos || d > 32768)
            return false;
        for (u32 i = 0; i < len; i++)
            put(s, s.window[(s.out_pos - d) & 0xFFFF]);
    }
}

bool stored(State& s) {
    // Discard the rest of the current byte, then LEN / NLEN.
    u32 drop = s.nbits & 7;
    getbits(s, drop);
    u32 len = getbits(s, 16), nlen = getbits(s, 16);
    if ((len ^ 0xFFFF) != nlen)
        return false;
    for (u32 i = 0; i < len; i++)
        put(s, static_cast<u8>(getbits(s, 8)));
    return true;
}

bool fixed(State& s, Huffman* lit, Huffman* dist) {
    u8 lengths[320];
    int i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    build(*lit, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    build(*dist, lengths, 30);
    return codes(s, *lit, *dist);
}

bool dynamic(State& s, Huffman* lit, Huffman* dist) {
    static const u8 ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    u32 nlen = getbits(s, 5) + 257, ndist = getbits(s, 5) + 1, ncode = getbits(s, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return false;
    u8 lengths[320] = {};
    for (u32 i = 0; i < ncode; i++)
        lengths[ORDER[i]] = static_cast<u8>(getbits(s, 3));
    Huffman lencode;
    build(lencode, lengths, 19);
    u32 index = 0;
    memset(lengths, 0, sizeof(lengths));
    while (index < nlen + ndist) {
        int sym = decode(s, lencode);
        if (sym < 0)
            return false;
        if (sym < 16) {
            lengths[index++] = static_cast<u8>(sym);
            continue;
        }
        u8 value = 0;
        u32 repeat;
        if (sym == 16) {
            if (index == 0)
                return false;
            value = lengths[index - 1];
            repeat = 3 + getbits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + getbits(s, 3);
        } else {
            repeat = 11 + getbits(s, 7);
        }
        if (index + repeat > nlen + ndist)
            return false;
        while (repeat--)
            lengths[index++] = value;
    }
    build(*lit, lengths, static_cast<int>(nlen));
    build(*dist, lengths + nlen, static_cast<int>(ndist));
    return codes(s, *lit, *dist);
}

} // namespace

bool is_gzip(const void* data, usize len) {
    auto* p = static_cast<const u8*>(data);
    return len >= 18 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 8;
}

long gunzip(const void* data, usize len, Sink sink, void* ctx) {
    if (!is_gzip(data, len))
        return -err::INVAL;
    auto* p = static_cast<const u8*>(data);
    u8 flags = p[3];
    usize pos = 10;
    if (flags & 4) { // FEXTRA
        pos += 2 + (p[pos] | (p[pos + 1] << 8));
    }
    if (flags & 8) // FNAME
        while (pos < len && p[pos++]) {}
    if (flags & 16) // FCOMMENT
        while (pos < len && p[pos++]) {}
    if (flags & 2) // FHCRC
        pos += 2;
    if (pos >= len)
        return -err::INVAL;
    auto* lit = static_cast<Huffman*>(kmalloc(sizeof(Huffman)));
    auto* dist = static_cast<Huffman*>(kmalloc(sizeof(Huffman)));
    u8* window = static_cast<u8*>(kmalloc(65536));
    if (!lit || !dist || !window) {
        kfree(lit);
        kfree(dist);
        kfree(window);
        return -err::NOMEM;
    }
    State s{p + pos, len - pos - 8, 0, 0, 0, window, 0, sink, ctx, false};
    bool ok = true;
    for (bool last = false; !last && ok;) {
        last = getbits(s, 1);
        u32 type = getbits(s, 2);
        ok = type == 0 ? stored(s) : type == 1 ? fixed(s, lit, dist) : type == 2 ? dynamic(s, lit, dist) : false;
    }
    // Whatever didn't fill a whole half yet.
    if (ok && !s.failed && (s.out_pos & 0x7FFF)) {
        u32 n = s.out_pos & 0x7FFF;
        u32 start = (s.out_pos - n) & 0xFFFF;
        if (!sink(ctx, window + start, n))
            s.failed = true;
    }
    kfree(lit);
    kfree(dist);
    kfree(window);
    return ok && !s.failed ? 0 : -err::INVAL;
}

} // namespace inflate
