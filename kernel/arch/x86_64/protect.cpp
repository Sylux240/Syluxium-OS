#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/protect.hpp>
#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <lib/spinlock.hpp>
#include <lib/string.hpp>

namespace protect {

bool g_smap = false;

namespace {

constexpr u64 CR4_SMEP = 1ull << 20, CR4_SMAP = 1ull << 21;
constexpr u64 RFLAGS_AC = 1ull << 18;

bool g_smep = false;
bool g_strict = false;
bool g_decided = false;

// Distinct code addresses that touched user memory outside a window
// (reported once each).
constexpr int MAX_SITES = 64;
u64 g_sites[MAX_SITES], g_site_addr[MAX_SITES], g_site_caller[MAX_SITES];
int g_site_count = 0, g_reported = 0;
IrqSpinlock g_sites_lock;

void decide() {
    auto l7 = arch::cpuid(0).eax >= 7 ? arch::cpuid(7, 0) : arch::CpuidResult{};
    const char* cmd = boot::cmdline();
    g_smep = (l7.ebx & (1u << 7)) && !strstr(cmd, "nosmep");
    g_smap = (l7.ebx & (1u << 20)) && !strstr(cmd, "nosmap");
    g_strict = strstr(cmd, "smap=strict") != nullptr;
    g_decided = true;
    klog::ok("Kernel protection: SMEP %s, SMAP %s%s", g_smep ? "on" : (l7.ebx & (1u << 7)) ? "off" : "unsupported",
             g_smap ? "on" : (l7.ebx & (1u << 20)) ? "off" : "unsupported", g_smap && g_strict ? " (strict)" : "");
}

} // namespace

void init_this_cpu() {
    if (!g_decided)
        decide();
    u64 cr4 = arch::read_cr4();
    if (g_smep)
        cr4 |= CR4_SMEP;
    if (g_smap)
        cr4 |= CR4_SMAP;
    arch::write_cr4(cr4);
}

bool smep() { return g_smep; }
bool smap() { return g_smap; }

bool smap_violation(u64 addr, u64 rip, u64* rflags, u64 caller) {
    if (!g_smap || (*rflags & RFLAGS_AC))
        return false;
    if (g_strict)
        panic("SMAP: the kernel touched user memory at %#lx (code at %#lx)", addr, rip);
    {
        // Only noted here: the faulting code may hold the log's lock (a
        // console write copying a user buffer). report_pending() prints.
        LockGuard guard(g_sites_lock);
        bool seen = false;
        for (int i = 0; i < g_site_count; i++)
            seen |= g_sites[i] == rip;
        if (!seen && g_site_count < MAX_SITES) {
            g_site_addr[g_site_count] = addr;
            g_site_caller[g_site_count] = caller;
            g_sites[g_site_count++] = rip;
        }
    }
    *rflags |= RFLAGS_AC; // let this kernel path finish its access
    return true;
}

void report_pending() {
    if (__atomic_load_n(&g_reported, __ATOMIC_ACQUIRE) == __atomic_load_n(&g_site_count, __ATOMIC_ACQUIRE))
        return;
    for (;;) {
        u64 rip, addr, caller;
        {
            LockGuard guard(g_sites_lock);
            if (g_reported >= g_site_count)
                return;
            rip = g_sites[g_reported];
            addr = g_site_addr[g_reported];
            caller = g_site_caller[g_reported];
            g_reported++;
        }
        klog::warn("SMAP: kernel code at %#lx (called from %#lx) touched user memory (%#lx) outside uaccess; allowed",
                   rip, caller, addr);
    }
}

} // namespace protect
