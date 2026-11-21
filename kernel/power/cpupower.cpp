#include <boot/limine_requests.hpp>
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
              MSR_AMD_CPPC_CAP1 = 0xC00102B0, MSR_AMD_CPPC_ENABLE = 0xC00102B1, MSR_AMD_CPPC_REQ = 0xC00102B3,
              MSR_AMD_PSTATE_LIMIT = 0xC0010061, MSR_AMD_PSTATE_CTL = 0xC0010062, MSR_AMD_PSTATE_DEF = 0xC0010064;

// AmdPstate: Zen without CPPC (Ryzen 1000-3000, many laptops whose firmware
// leaves CPPC off): the hardware P-states P0 (fastest) .. Pn, picked by load.
enum class Driver { None, Hwp, Cppc, Legacy, AmdPstate };

bool g_detected = false;
Driver g_driver = Driver::None;
bool g_intel = false, g_amd = false;
bool g_aperf = false;
bool g_mwait_deep = false;
bool g_deep_idle_ok = false; // the APIC timer and TSC keep running in deep C-states (ARAT + invariant TSC)
bool g_idle_hlt = false;     // idle=hlt
u16 g_io_cstate_port = 0;
u64 g_io_cstate_min_ns = 0;
u32 g_mwait_hint = 0;
const char* g_cstate_name = "C1 (hlt)";
bool g_tickless = false;
// Frequencies in kHz.
u32 g_min_khz = 0, g_max_khz = 0, g_base_khz = 0;
// Legacy governor ratios (x 100 MHz).
u32 g_min_ratio = 0, g_base_ratio = 0, g_turbo_ratio = 0;
bool g_epp = false;
// AMD hardware P-states: frequency of each (kHz), P0 first.
u32 g_amd_pstate_khz[8] = {};
u32 g_amd_pstates = 0;
constexpr u32 EPP_BALANCE_PERFORMANCE = 0x80;

u32 tsc_khz() { return static_cast<u32>(clock::tsc_frequency_hz() / 1000); }

// Zen P-state definitions: frequency = FID / DID x 200 MHz (DID in 1/8 steps).
void detect_amd_pstates() {
    u64 limit;
    if (!arch::rdmsr_safe(MSR_AMD_PSTATE_LIMIT, &limit)) {
        klog::info("CPU power: AMD P-state registers not available (virtual machine?)");
        return;
    }
    u32 max_index = (limit >> 4) & 7;
    for (u32 i = 0; i <= max_index; i++) {
        u64 def;
        if (!arch::rdmsr_safe(MSR_AMD_PSTATE_DEF + i, &def) || !(def >> 63))
            break;
        u32 fid = def & 0xFF, did = (def >> 8) & 0x3F;
        if (!fid || !did)
            break;
        g_amd_pstate_khz[g_amd_pstates++] = fid * 200000 / did;
    }
    u64 ctl;
    if (g_amd_pstates >= 2 && arch::rdmsr_safe(MSR_AMD_PSTATE_CTL, &ctl)) {
        g_driver = Driver::AmdPstate;
        g_max_khz = g_amd_pstate_khz[0];
        g_min_khz = g_amd_pstate_khz[g_amd_pstates - 1];
        g_base_khz = g_max_khz;
    }
}

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

    g_deep_idle_ok = arat && invariant_tsc;
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
    if (g_driver == Driver::None && g_amd && family >= 0x17)
        detect_amd_pstates();
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
    case Driver::AmdPstate:
        arch::wrmsr_safe(MSR_AMD_PSTATE_CTL, 0);
        cpu->cur_ratio = 0; // P-state index
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
    bool ondemand = g_driver == Driver::Legacy || g_driver == Driver::AmdPstate;
    const char* driver = g_driver == Driver::Hwp    ? "intel_pstate"
                         : g_driver == Driver::Cppc ? "amd-pstate-epp"
                         : g_driver == Driver::AmdPstate ? "acpi-cpufreq"
                                                    : "syluxium-ondemand";
    const char* governor = ondemand ? "ondemand" : "powersave";
    u32 lo = g_min_khz ? g_min_khz : tsc_khz(), hi = g_max_khz ? g_max_khz : tsc_khz();
    switch (field) {
    case 0: out.printf("%s\n", driver); break;
    case 1: out.printf("%s\n", governor); break;
    case 2: out.printf("%s\n", ondemand ? "ondemand" : "performance powersave"); break;
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
    if (!g_detected) {
        detect();
        if (strstr(boot::cmdline(), "idle=hlt")) {
            g_idle_hlt = true;
            g_mwait_deep = false;
            g_cstate_name = "C1 (hlt, idle=hlt)";
        }
    }
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
    g_tickless = !strstr(boot::cmdline(), "nohz=off");
    const char* driver = g_driver == Driver::Hwp    ? "Intel HWP (hardware P-states)"
                         : g_driver == Driver::Cppc ? "AMD CPPC"
                         : g_driver == Driver::Legacy ? "SpeedStep, load-based governor"
                         : g_driver == Driver::AmdPstate ? "AMD P-states, load-based governor"
                                                      : "firmware-controlled";
    if (g_min_khz)
        klog::ok("CPU power: %s, %u-%u MHz; idle %s, %s", driver, g_min_khz / 1000, g_max_khz / 1000,
                 g_cstate_name, g_tickless ? "tickless" : "periodic tick (nohz=off)");
    else
        klog::ok("CPU power: frequency %s; idle %s, %s", driver, g_cstate_name,
                 g_tickless ? "tickless" : "periodic tick (nohz=off)");
}

bool tickless_enabled() { return g_tickless; }

void set_io_cstate(u16 port, u32 latency_us, int acpi_type) {
    if (!g_amd || g_idle_hlt || g_mwait_deep || !g_deep_idle_ok || !port)
        return;
    if (acpi_type != 2 || latency_us > 1000)
        return; // C3 and deeper would need bus-master handling; not worth the risk
    // Only when the CPU will likely sleep well beyond the exit latency.
    g_io_cstate_min_ns = static_cast<u64>(latency_us) * 1000 * 10;
    if (g_io_cstate_min_ns < 200'000)
        g_io_cstate_min_ns = 200'000;
    __atomic_store_n(&g_io_cstate_port, port, __ATOMIC_RELEASE);
    static char name[40];
    snprintf(name, sizeof(name), "ACPI C2 (I/O port %#x, %u us)", port, latency_us);
    g_cstate_name = name;
    klog::ok("CPU power: idle CPUs use %s", name);
}

void idle(u64 expected_ns) {
    u16 io_port = __atomic_load_n(&g_io_cstate_port, __ATOMIC_ACQUIRE);
    if (io_port && expected_ns >= g_io_cstate_min_ns) {
        // Like Linux's acpi_idle: the port read enters the C-state with
        // interrupts off; a pending interrupt still wakes the core.
        if (!this_cpu()->need_resched) {
            u64 before = clock::now_ns();
            arch::inb(io_port);
            // A port that doesn't actually stop the core would make idle CPUs
            // spin: after many instant returns in a row, fall back to hlt.
            static u32 instant = 0;
            if (clock::now_ns() - before < 2000) {
                if (++instant == 5000) {
                    __atomic_store_n(&g_io_cstate_port, 0, __ATOMIC_RELEASE);
                    g_cstate_name = "C1 (hlt; the ACPI C2 port did not idle)";
                }
            } else {
                instant = 0;
            }
        }
        asm volatile("sti" ::: "memory");
        return;
    }
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
    } else if (g_driver == Driver::AmdPstate) {
        // ondemand over the hardware P-states: P0 above 80% load.
        u32 slowest = g_amd_pstates - 1;
        u32 index = load >= 80 ? 0 : slowest - slowest * load / 80;
        if (index != cpu->cur_ratio) {
            arch::wrmsr_safe(MSR_AMD_PSTATE_CTL, index);
            cpu->cur_ratio = index;
        }
        if (!g_aperf)
            cpu->cur_khz = g_amd_pstate_khz[index];
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
