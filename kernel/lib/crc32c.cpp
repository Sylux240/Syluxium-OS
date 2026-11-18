#include <lib/crc32c.hpp>

namespace {

u32 g_table[256];
int g_mode = 0; // 0 = not decided yet, 1 = table, 2 = SSE4.2 instruction

void init() {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        g_table[i] = c;
    }
    u32 eax = 1, ebx, ecx, edx;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "c"(0));
    g_mode = (ecx & (1u << 20)) ? 2 : 1; // CPUID.1:ECX.SSE4_2
}

__attribute__((target("sse4.2"))) u32 crc_hw(u32 crc, const u8* p, usize len) {
    u64 c = crc;
    while (len && (reinterpret_cast<uptr>(p) & 7)) {
        c = __builtin_ia32_crc32qi(static_cast<u32>(c), *p++);
        len--;
    }
    while (len >= 8) {
        u64 v;
        __builtin_memcpy(&v, p, 8);
        c = __builtin_ia32_crc32di(c, v);
        p += 8;
        len -= 8;
    }
    while (len--)
        c = __builtin_ia32_crc32qi(static_cast<u32>(c), *p++);
    return static_cast<u32>(c);
}

} // namespace

u32 crc32c(u32 crc, const void* data, usize len) {
    if (!g_mode)
        init();
    auto* p = static_cast<const u8*>(data);
    if (g_mode == 2)
        return crc_hw(crc, p, len);
    while (len--)
        crc = (crc >> 8) ^ g_table[(crc ^ *p++) & 0xFF];
    return crc;
}
