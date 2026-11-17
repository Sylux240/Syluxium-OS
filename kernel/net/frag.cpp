// IP fragment reassembly, shared by IPv4 (RFC 791) and IPv6 (RFC 8200).
//
// A few datagrams can be in reassembly at once. Each keeps a 64 KiB
// buffer and a bitmap of the 8-byte blocks received so far; the datagram
// is complete once the last fragment (MF clear) fixed its length and every
// block below it has arrived. Overlapping fragments simply overwrite
// (RFC 5722 says to drop the datagram for IPv6; harmless for our use).
// Incomplete datagrams are dropped after 30 seconds.

#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <time/clock.hpp>

namespace net {

namespace {

constexpr usize SLOTS = 8;
constexpr usize BLOCKS = (MAX_IP_PAYLOAD + 7) / 8;
constexpr u64 TIMEOUT_MS = 30000;

struct Reassembly {
    bool used = false;
    FragKey key{};
    u8* data = nullptr;          // MAX_IP_PAYLOAD bytes
    u8 have[(BLOCKS + 7) / 8] = {};
    usize total = 0;             // 0 until the last fragment arrived
    usize blocks_received = 0;
    u64 started_ms = 0;
};

Reassembly g_slots[SLOTS];

bool same_key(const FragKey& a, const FragKey& b) {
    return a.v6 == b.v6 && a.id == b.id && a.proto == b.proto && memcmp(a.src, b.src, 16) == 0 &&
           memcmp(a.dst, b.dst, 16) == 0;
}

void release(Reassembly& r) {
    kfree(r.data);
    r = Reassembly{};
}

Reassembly* slot_for(const FragKey& key) {
    Reassembly* oldest = &g_slots[0];
    for (auto& r : g_slots) {
        if (r.used && same_key(r.key, key))
            return &r;
    }
    for (auto& r : g_slots) {
        if (!r.used) {
            oldest = &r;
            break;
        }
        if (r.started_ms < oldest->started_ms)
            oldest = &r;
    }
    release(*oldest); // all busy: the oldest datagram loses
    oldest->data = static_cast<u8*>(kmalloc(MAX_IP_PAYLOAD));
    if (!oldest->data)
        return nullptr;
    oldest->used = true;
    oldest->key = key;
    oldest->started_ms = clock::now_ms();
    return oldest;
}

} // namespace

u8* frag_add(const FragKey& key, usize offset, bool more, const u8* data, usize len, usize* total) {
    if (offset + len > MAX_IP_PAYLOAD || (more && (len == 0 || len % 8)))
        return nullptr; // only the last fragment may have a length that isn't a multiple of 8
    Reassembly* r = slot_for(key);
    if (!r)
        return nullptr;
    if (!more) {
        if (r->total && r->total != offset + len) { // conflicting ends: give up on it
            release(*r);
            return nullptr;
        }
        r->total = offset + len;
    }
    memcpy(r->data + offset, data, len);
    for (usize b = offset / 8; b < (offset + len + 7) / 8; b++) {
        if (!(r->have[b / 8] & (1u << (b % 8)))) {
            r->have[b / 8] |= static_cast<u8>(1u << (b % 8));
            r->blocks_received++;
        }
    }
    if (!r->total || r->blocks_received < (r->total + 7) / 8)
        return nullptr;
    for (usize b = 0; b < (r->total + 7) / 8; b++) // blocks past the end don't count
        if (!(r->have[b / 8] & (1u << (b % 8))))
            return nullptr;
    u8* whole = r->data;
    *total = r->total;
    r->data = nullptr;
    release(*r);
    return whole;
}

void frag_timer(u64 now_ms) {
    for (auto& r : g_slots)
        if (r.used && now_ms - r.started_ms >= TIMEOUT_MS)
            release(r);
}

} // namespace net
