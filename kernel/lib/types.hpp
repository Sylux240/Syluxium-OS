#pragma once

// Freestanding headers provided by the compiler itself.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using usize = size_t;
using isize = ptrdiff_t;
using uptr  = uintptr_t;

#define PACKED      __attribute__((packed))
#define ALIGNED(n)  __attribute__((aligned(n)))
#define PRINTF_LIKE(fmt, args) __attribute__((format(printf, fmt, args)))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

template <typename T, usize N>
constexpr usize array_size(const T (&)[N]) { return N; }

template <typename T>
constexpr T align_up(T value, T align) { return (value + align - 1) & ~(align - 1); }

template <typename T>
constexpr T align_down(T value, T align) { return value & ~(align - 1); }

template <typename T>
constexpr const T& min(const T& a, const T& b) { return b < a ? b : a; }

template <typename T>
constexpr const T& max(const T& a, const T& b) { return a < b ? b : a; }

constexpr u64 KiB = 1024;
constexpr u64 MiB = 1024 * KiB;
constexpr u64 GiB = 1024 * MiB;
