#include <lib/string.hpp>

// rep movsb / rep stosb are fast on every CPU with ERMS (Ivy Bridge+, Zen+)
// and, unlike plain C loops, can't be turned back into calls to themselves
// by the optimizer.

extern "C" void* memcpy(void* __restrict dst, const void* __restrict src, usize n) {
    void* ret = dst;
    asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

extern "C" void* memset(void* dst, int value, usize n) {
    void* ret = dst;
    asm volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(value) : "memory");
    return ret;
}

extern "C" void* memmove(void* dst, const void* src, usize n) {
    auto d = static_cast<u8*>(dst);
    auto s = static_cast<const u8*>(src);
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);

    // Overlapping with dst after src: copy backwards.
    d += n - 1;
    s += n - 1;
    asm volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

extern "C" int memcmp(const void* a, const void* b, usize n) {
    auto x = static_cast<const u8*>(a);
    auto y = static_cast<const u8*>(b);
    for (usize i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

extern "C" usize strlen(const char* s) {
    usize n = 0;
    while (s[n])
        n++;
    return n;
}

extern "C" int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return static_cast<u8>(*a) - static_cast<u8>(*b);
}

extern "C" int strncmp(const char* a, const char* b, usize n) {
    for (usize i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i])
            return static_cast<u8>(a[i]) - static_cast<u8>(b[i]);
    }
    return 0;
}
