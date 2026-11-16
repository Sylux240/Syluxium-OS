#pragma once

#include <lib/types.hpp>

// Safe access to user memory from system calls.
//
// Every user pointer is checked against the process's mappings, and pages
// that haven't been touched yet are faulted in *before* the kernel copies,
// so the kernel never takes an unexpected page fault on a user address.
namespace uaccess {

// Validate [ptr, ptr+len) for reading (or writing) and fault it in.
// Returns 0 or -EFAULT. After success the range may be accessed directly.
long check(const void* ptr, usize len, bool write);

long copy_from(void* dst, const void* user_src, usize len);
long copy_to(void* user_dst, const void* src, usize len);

// Copy a NUL-terminated string; returns its length, -EFAULT or -ENAMETOOLONG.
long string_from(const char* user_src, char* dst, usize max_len);

template <typename T>
long get(T* out, const T* user_ptr) { return copy_from(out, user_ptr, sizeof(T)); }
template <typename T>
long put(T* user_ptr, const T& value) { return copy_to(user_ptr, &value, sizeof(T)); }

} // namespace uaccess
