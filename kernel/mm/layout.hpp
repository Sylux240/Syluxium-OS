#pragma once

#include <lib/types.hpp>

// Virtual address space layout (x86_64, 4-level paging, 48-bit addresses)
//
//   0x0000000000000000 - 0x00007fffffffffff   user space (128 TiB, per process)
//   0xffff800000000000 - ...                  direct map of all RAM ("HHDM")
//   0xffffc00000000000 - 0xffffdfffffffffff   vmalloc + MMIO mappings (32 TiB)
//   0xffffffff80000000 - 0xffffffffffffffff   kernel image (-mcmodel=kernel)
//
// Everything from 0xffff800000000000 up is the kernel half, identical in
// every address space.

namespace mm {

constexpr u64 PAGE_SIZE = 4096;
constexpr unsigned PAGE_SHIFT = 12;
constexpr u64 LARGE_PAGE_SIZE = 2 * MiB;
constexpr u64 HUGE_PAGE_SIZE = 1 * GiB;

constexpr uptr USER_END      = 0x0000800000000000;
constexpr uptr KERNEL_HALF   = 0xffff800000000000;
constexpr uptr VMALLOC_START = 0xffffc00000000000;
constexpr uptr VMALLOC_END   = 0xffffe00000000000;
constexpr uptr KERNEL_BASE   = 0xffffffff80000000;

// Start of the direct map; set from the bootloader's answer in pmm::init().
inline u64 g_hhdm_offset = 0;

template <typename T = void>
inline T* phys_to_virt(u64 phys) {
    return reinterpret_cast<T*>(phys + g_hhdm_offset);
}

// Only valid for pointers into the direct map (kmalloc'd memory, page tables...).
inline u64 direct_virt_to_phys(const void* virt) {
    return reinterpret_cast<uptr>(virt) - g_hhdm_offset;
}

inline bool is_direct_mapped(const void* virt) {
    auto addr = reinterpret_cast<uptr>(virt);
    return addr >= g_hhdm_offset && addr < VMALLOC_START;
}

inline bool is_vmalloc_address(const void* virt) {
    auto addr = reinterpret_cast<uptr>(virt);
    return addr >= VMALLOC_START && addr < VMALLOC_END;
}

} // namespace mm
