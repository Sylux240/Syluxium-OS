#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <fs/genfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <power/cpupower.hpp>
#include <time/clock.hpp>

namespace cpupower {

namespace {

constexpr u32 MSR_MPERF = 0xE7, MSR_APERF = 0xE8, MSR_PLATFORM_INFO = 0xCE, MSR_PERF_CTL = 0x199,
              MSR_MISC_ENABLE = 0x1A0, MSR_TURBO_RATIO = 0x1AD, MSR_ENERGY_PERF_BIAS = 0x1B0,
              MSR_PM_ENABLE = 0x770, MSR_HWP_CAPABILITIES = 0x771, MSR_HWP_REQUEST = 0x774,
              MSR_AMD_CPPC_CAP1 = 0xC00102B0, MSR_AMD_CPPC_ENABLE = 0xC00102B1, MSR_AMD_CPPC_REQ = 0xC00102B3;

enum class Driver { None, Hwp, Cppc, Legacy };

bool g_detected = false;
Driver g_driver = Driver::None;
bool g_intel = false, g_amd = false;
bool g_aperf = false;
bool g_mwait_deep = false;
u32 g_mwait_hint = 0;
const char* g_cstate_name = "C1 (hlt)";
bool g_tickless = false;
// Frequencies in kHz.
u32 g_min_khz = 0, g_max_khz = 0, g_base_khz = 0;
// Legacy governor ratios (x 100 MHz).
u32 g_min_ratio = 0, g_base_ratio = 0, g_turbo_ratio = 0;
bool g_epp = false;
constexpr u32 EPP_BALANCE_PERFORMANCE = 0x80;

u32 tsc_khz() { return static_cast<u32>(clock::tsc_frequency_hz() / 1000); }

void detect() {
    auto v = arch::cpuid(0);
    g_intel = v.ebx == 0x756E6547; // "Genu"ineIntel
    g_amd = v.ebx == 0x68747541;   // "Auth"enticAMD
    u32 max_leaf = v.eax;
    auto l1 = arch::cpuid(1);
    u32 family = (l1.eax >> 8) & 0xF, model = (l1.eax >> 4) & 0xF;
    if (family == 6 || family == 0xF)
        model |= ((l1.eax >> 16) & 0xF) << 4;
    if (family == 0xF)
        family += (l1.eax >> 20) & 0xFF;
    u32 l6a = 0, l6c = 0;
    if (max_leaf >= 6) {
        auto l6 = arch::cpuid(6);
        l6a = l6.eax;
        l6c = l6.ecx;
    }
    g_aperf = l6c & 1;
    bool arat = l6a & (1u << 2);
    u32 ext_max = arch::cpuid(0x80000000).eax;
    bool invariant_tsc = ext_max >= 0x80000007 && (arch::cpuid(0x80000007).edx & (1u << 8));

    // Deep idle through MWAIT (Intel lists its C-states in leaf 5).
    bool monitor = l1.ecx & (1u << 3);
    if (g_intel && monitor && max_leaf >= 5 && arat && invariant_tsc) {
        auto l5 = arch::cpuid(5);
        bool extensions = (l5.ecx & 1) && (l5.ecx & 2); // enumeration + break on interrupt
        if (extensions) {
            for (int c = 7; c >= 1; c--) { // EDX: sub-state counts for C0..C7, 4 bits each
                u32 subs = (l5.edx >> (4 * c)) & 0xF;
                if (c >= 2 && subs) {
                    g_mwait_hint = static_cast<u32>((c - 1) << 4) | (subs - 1);
                    g_mwait_deep = true;
                    static char name[24];
                    snprintf(name, sizeof(name), "MWAIT C%d (hint %#x)", c, g_mwait_hint);
                    g_cstate_name = name;
                    break;
                }
            }
        }
    }

    // Frequency.
    if (g_intel && (l6a & (1u << 7))) {
        g_driver = Driver::Hwp;
        g_epp = l6a & (1u << 10);
    } else if (g_amd && ext_max >= 0x80000008 && (arch::cpuid(0x80000008).ebx & (1u << 27))) {
        g_driver = Driver::Cppc;
        g_epp = true;
    } else if (g_intel && family == 6 && model >= 0x1A && (l1.ecx & (1u << 7)) &&
               (arch::rdmsr(MSR_MISC_ENABLE) & (1u << 16))) {
        // Enhanced SpeedStep on, Nehalem or later (PLATFORM_INFO / turbo MSRs exist).
        u64 pi = arch::rdmsr(MSR_PLATFORM_INFO);
        g_base_ratio = (pi >> 8) & 0xFF;
        g_min_ratio = (pi >> 40) & 0xFF;
        g_turbo_ratio = g_base_ratio;
        bool turbo = (l6a & 2) && !(arch::rdmsr(MSR_MISC_ENABLE) & (1ull << 38));
        if (turbo) {
            u32 t = arch::rdmsr(MSR_TURBO_RATIO) & 0xFF;
            if (t > g_base_ratio)
                g_turbo_ratio = t;
        }
        if (g_base_ratio && g_min_ratio && g_min_ratio <= g_base_ratio) {
            g_driver = Driver::Legacy;
            g_min_khz = g_min_ratio * 100000;
            g_max_khz = g_turbo_ratio * 100000;
            g_base_khz = g_base_ratio * 100000;
        }
    }
    g_detected = true;
}

void setup_this_cpu() {
    Cpu* cpu = this_cpu();
    switch (g_driver) {
    case Driver::Hwp: {
        arch::wrmsr(MSR_PM_ENABLE, 1);
        u64 caps = arch::rdmsr(MSR_HWP_CAPABILITIES);
        u32 highest = caps & 0xFF, guaranteed = (caps >> 8) & 0xFF, lowest = (caps >> 24) & 0xFF;
        // Autonomous: min..max range, no desired level, balanced preference.
        u64 req = lowest | (static_cast<u64>(highest) << 8);
        if (g_epp)
            req |= static_cast<u64>(EPP_BALANCE_PERFORMANCE) << 24;
        else
            arch::wrmsr(MSR_ENERGY_PERF_BIAS, 6); // "normal"
        arch::wrmsr(MSR_HWP_REQUEST, req);
        // Intel performance levels are (close to) 100 MHz steps.
        g_min_khz = lowest * 100000;
        g_max_khz = highest * 100000;
        g_base_khz = guaranteed * 100000;
        break;
    }
    case Driver::Cppc: {
        arch::wrmsr(MSR_AMD_CPPC_ENABLE, 1);
        u64 cap = arch::rdmsr(MSR_AMD_CPPC_CAP1);
        u32 highest = (cap >> 24) & 0xFF, nominal = (cap >> 16) & 0xFF, lowest = cap & 0xFF;
        u64 req = highest | (static_cast<u64>(lowest) << 8) | (static_cast<u64>(EPP_BALANCE_PERFORMANCE) << 24);
        arch::wrmsr(MSR_AMD_CPPC_REQ, req);
        // Performance levels -> kHz through the nominal level = base (TSC) clock.
        if (nominal) {
            g_base_khz = tsc_khz();
            g_max_khz = static_cast<u32>(static_cast<u64>(g_base_khz) * highest / nominal);
            g_min_khz = static_cast<u32>(static_cast<u64>(g_base_khz) * lowest / nominal);
        }
        break;
    }
    case Driver::Legacy:
        arch::wrmsr(MSR_PERF_CTL, static_cast<u64>(g_base_ratio) << 8);
        cpu->cur_ratio = g_base_ratio;
        break;
    case Driver::None:
        break;
    }
    if (g_aperf) {
        cpu->last_aperf = arch::rdmsr(MSR_APERF);
        cpu->last_mperf = arch::rdmsr(MSR_MPERF);
    }
    cpu->cur_khz = g_base_khz ? g_base_khz : tsc_khz();
}

// ---- sysfs ----

bool gen_cpufreq(genfs::TextBuffer& out, const void* ctx) {
    u32 cpu_index = static_cast<u32>(reinterpret_cast<uptr>(ctx) >> 8);
    u32 field = static_cast<u32>(reinterpret_cast<uptr>(ctx) & 0xFF);
    Cpu* cpu = percpu::get(cpu_index);
    const char* driver = g_driver == Driver::Hwp    ? "intel_pstate"
                         : g_driver == Driver::Cppc ? "amd-pstate-epp"
                                                    : "syluxium-ondemand";
    const char* governor = g_driver == Driver::Legacy ? "ondemand" : "powersave";
    u32 lo = g_min_khz ? g_min_khz : tsc_khz(), hi = g_max_khz ? g_max_khz : tsc_khz();
    switch (field) {
    case 0: out.printf("%s\n", driver); break;
    case 1: out.printf("%s\n", governor); break;
    case 2: out.printf("%s\n", g_driver == Driver::Legacy ? "ondemand" : "performance powersave"); break;
    case 3: out.printf("%u\n", lo); break;
    case 4: out.printf("%u\n", hi); break;
    case 5: out.printf("%u\n", lo); break;
    case 6: out.printf("%u\n", hi); break;
    case 7: out.printf("%u\n", cpu ? cpu->cur_khz : 0); break;
    case 8: out.printf("%u\n", g_base_khz ? g_base_khz : tsc_khz()); break;
    case 9: out.printf("%s\n", g_epp ? "balance_performance" : "default"); break;
    case 10: out.printf("%u\n", cpu_index); break;
    }
    return true;
}

} // namespace

void init_cpu() {
    if (!g_detected)
        detect();
    setup_this_cpu();
}

void publish() {
    static const char* const names[] = {"scaling_driver", "scaling_governor", "scaling_available_governors",
                                        "cpuinfo_min_freq", "cpuinfo_max_freq", "scaling_min_freq",
                                        "scaling_max_freq", "scaling_cur_freq", "base_frequency",
                                        "energy_performance_preference", "affected_cpus"};
    u32 cpus = percpu::online_count();
    for (u32 i = 0; i < cpus; i++) {
        char dir[80], path[128];
        snprintf(dir, sizeof(dir), "/sys/devices/system/cpu/cpu%u/cpufreq", i);
        vfs::mkdir_p(dir, 0755);
        for (u32 f = 0; f < 11; f++) {
            snprintf(path, sizeof(path), "%s/%s", dir, names[f]);
            vfs::add_node(path, new genfs::GeneratedFile(gen_cpufreq,
                                                         reinterpret_cast<const void*>(static_cast<uptr>((i << 8) | f))));
        }
    }
    g_tickless = true;
    const char* driver = g_driver == Driver::Hwp    ? "Intel HWP (hardware P-states)"
                         : g_driver == Driver::Cppc ? "AMD CPPC"
                         : g_driver == Driver::Legacy ? "SpeedStep, load-based governor"
                                                      : "firmware-controlled";
    if (g_min_khz)
        klog::ok("CPU power: %s, %u-%u MHz; idle %s, tickless", driver, g_min_khz / 1000, g_max_khz / 1000,
                 g_cstate_name);
    else
        klog::ok("CPU power: frequency %s; idle %s, tickless", driver, g_cstate_name);
}

bool tickless_enabled() { return g_tickless; }

void idle(u64 expected_ns) {
    if (g_mwait_deep && expected_ns >= 2'000'000) {
        Cpu* cpu = this_cpu();
        // Arm the monitor on need_resched, then sti; mwait (sti's one
        // instruction delay keeps a wakeup from slipping in between).
        asm volatile("monitor" ::"a"(&cpu->need_resched), "c"(0), "d"(0));
        if (!cpu->need_resched) {
            asm volatile("sti; mwait" ::"a"(g_mwait_hint), "c"(1) : "memory");
            return;
        }
        asm volatile("sti" ::: "memory");
        return;
    }
    asm volatile("sti; hlt" ::: "memory");
}

void tick(Cpu* cpu) {
    constexpr u64 WINDOW = 20; // ms
    if (cpu->window_ticks < WINDOW) // counted by the scheduler tick
        return;
    u64 busy = cpu->window_ticks - min(cpu->window_idle, cpu->window_ticks);
    u32 load = static_cast<u32>(busy * 100 / cpu->window_ticks);
    cpu->window_ticks = 0;
    cpu->window_idle = 0;
    if (g_driver == Driver::Legacy) {
        // ondemand: full speed above 80% load, else proportional.
        u32 ratio = load >= 80 ? g_turbo_ratio
                               : g_min_ratio + (g_base_ratio - g_min_ratio) * load / 80;
        if (ratio != cpu->cur_ratio) {
            arch::wrmsr(MSR_PERF_CTL, static_cast<u64>(ratio) << 8);
            cpu->cur_ratio = ratio;
        }
        if (!g_aperf)
            cpu->cur_khz = ratio * 100000;
    }
    if (g_aperf) {
        // Effective frequency while running: base clock x APERF/MPERF.
        u64 a = arch::rdmsr(MSR_APERF), m = arch::rdmsr(MSR_MPERF);
        u64 da = a - cpu->last_aperf, dm = m - cpu->last_mperf;
        cpu->last_aperf = a;
        cpu->last_mperf = m;
        if (dm)
            cpu->cur_khz = static_cast<u32>(static_cast<u64>(tsc_khz()) * da / dm);
    }
}

} // namespace cpupower
