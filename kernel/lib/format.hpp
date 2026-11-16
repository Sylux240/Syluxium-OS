#pragma once

#include <lib/types.hpp>

// printf-style formatting engine shared by kprintf, snprintf, etc.
//
// Supported: %d %i %u %x %X %o %b %p %s %c %%
// Flags: - + space # 0, width, .precision (both may be *)
// Length: hh h l ll z t j
using FormatSink = void (*)(char c, void* ctx);

int vformat(FormatSink sink, void* ctx, const char* fmt, va_list args);

int vsnprintf(char* buf, usize size, const char* fmt, va_list args);
int snprintf(char* buf, usize size, const char* fmt, ...) PRINTF_LIKE(3, 4);
