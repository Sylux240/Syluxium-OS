#pragma once

#include <lib/types.hpp>

[[noreturn]] void panic(const char* fmt, ...) PRINTF_LIKE(1, 2);

#define KASSERT(cond)                                                                  \
    do {                                                                               \
        if (unlikely(!(cond)))                                                         \
            panic("assertion failed: %s\n  at %s:%d", #cond, __FILE__, __LINE__);      \
    } while (0)
