#pragma once

#include <lib/types.hpp>

// x86_64 page table entry bits (same layout at every level, except where noted).
namespace paging {

constexpr u64 PTE_PRESENT   = 1ull << 0;
constexpr u64 PTE_WRITABLE  = 1ull << 1;
constexpr u64 PTE_USER      = 1ull << 2;
constexpr u64 PTE_PWT       = 1ull << 3;  // PAT index bit 0
constexpr u64 PTE_PCD       = 1ull << 4;  // PAT index bit 1
constexpr u64 PTE_ACCESSED  = 1ull << 5;
constexpr u64 PTE_DIRTY     = 1ull << 6;
constexpr u64 PTE_HUGE      = 1ull << 7;  // PD/PDPT level: maps a 2 MiB / 1 GiB page
constexpr u64 PTE_PAT_4K    = 1ull << 7;  // PT level: PAT index bit 2
constexpr u64 PTE_GLOBAL    = 1ull << 8;  // survives CR3 switches (kernel mappings)
constexpr u64 PTE_PAT_LARGE = 1ull << 12; // PD/PDPT level huge page: PAT index bit 2
constexpr u64 PTE_NX        = 1ull << 63; // no execute

constexpr u64 PTE_ADDR_MASK = 0x000ffffffffff000;

// Our PAT layout (same as Limine's, plus defined entries 6 and 7).
//   0 WB   1 WT   2 UC-   3 UC   4 WP   5 WC   6 UC-   7 UC
constexpr u64 PAT_VALUE = 0x0007010500070406;
constexpr u32 MSR_PAT = 0x277;

} // namespace paging
