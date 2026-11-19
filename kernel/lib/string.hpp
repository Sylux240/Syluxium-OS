#pragma once

#include <lib/types.hpp>

// The compiler may emit calls to memcpy/memset/memmove/memcmp on its own,
// so these must have C linkage and exactly these names.
extern "C" {
void* memcpy(void* __restrict dst, const void* __restrict src, usize n);
void* memmove(void* dst, const void* src, usize n);
void* memset(void* dst, int value, usize n);
int   memcmp(const void* a, const void* b, usize n);
usize strlen(const char* s);
int   strcmp(const char* a, const char* b);
int   strncmp(const char* a, const char* b, usize n);
extern "C" char* strstr(const char* haystack, const char* needle); // in linuxkpi/core.cpp
}
