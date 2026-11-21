// ACPI devices through the AML interpreter (uACPI): the power button, the
// battery, the AC adapter and the lid. Batteries and adapters appear in
// /sys/class/power_supply like on Linux (TDE's battery monitor reads them).

#include <core/bootlog.hpp>
#include <boot/limine_requests.hpp>
#include <acpi/acpi.hpp>
#include <acpi/devices.hpp>
#include <acpi/ec.hpp>
#include <acpi/power.hpp>
#include <block/block.hpp>
#include <core/log.hpp>
#include <fs/ext2.hpp>
#include <fs/genfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

#include <arch/x86_64/cpu.hpp>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/notify.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace acpi {

namespace {

bool g_ready = false;

// ---- power button: an orderly shutdown ----

bool g_shutting_down = false;

void shutdown_thread(void*) {
    klog::info("Power button: shutting down");
    proc::for_each([](proc::Process* p, void*) { signal::send(p, signal::SIGTERM); }, nullptr);
    for (int i = 0; i < 50; i++) { // up to 5 s for programs to save and quit
        sched::sleep_ms(100);
        bool any = false;
        proc::for_each([](proc::Process* p, void* a) {
            if (p->state != proc::State::Zombie)
                *static_cast<bool*>(a) = true;
        }, &any);
        if (!any)
            break;
    }
    proc::for_each([](proc::Process* p, void*) { signal::send(p, signal::SIGKILL); }, nullptr);
    bootlog::save();
    block::sync_all();
    ext2::shutdown_all();
    block::sync_all();
    power_off();
}

void request_shutdown() {
    if (__atomic_exchange_n(&g_shutting_down, true, __ATOMIC_ACQ_REL))
        return;
    sched::spawn("shutdown", shutdown_thread, nullptr, -1, true);
}

uacpi_interrupt_ret fixed_power_button(uacpi_handle) {
    request_shutdown(); // spawns a thread: fine from the SCI handler
    return UACPI_INTERRUPT_HANDLED;
}

uacpi_status power_button_notify(uacpi_handle, uacpi_namespace_node*, uacpi_u64 value) {
    if (value == 0x80) // "button pressed"
        request_shutdown();
    return UACPI_STATUS_OK;
}

// ---- battery / AC adapter / lid ----
//
// Like Linux's /sys/class/power_supply: one directory per battery (BATn)
// and adapter (AC), with the attributes and "uevent" properties upower and
// TDE's hardware library (tdepowersave's battery icon) read, and a
// "subsystem" link so libudev classifies them.

constexpr int MAX_BATTERIES = 4;
uacpi_namespace_node* g_batteries[MAX_BATTERIES];
int g_battery_count = 0;
uacpi_namespace_node* g_ac = nullptr;
uacpi_namespace_node* g_lid = nullptr;
sched::Mutex g_battery_lock; // AML evaluation results are cached under it

u64 package_int(uacpi_object_array& a, usize i) {
    u64 v = 0;
    if (i < a.count)
        uacpi_object_get_integer(a.objects[i], &v);
    return v;
}

void package_string(uacpi_object_array& a, usize i, char* out, usize size) {
    out[0] = 0;
    uacpi_data_view v;
    if (i < a.count && uacpi_object_get_string_or_buffer(a.objects[i], &v) == UACPI_STATUS_OK && v.length) {
        usize n = 0;
        for (; n < v.length && n + 1 < size && v.const_text[n]; n++)
            out[n] = (v.const_text[n] >= 32 && v.const_text[n] < 127) ? v.const_text[n] : '?';
        out[n] = 0;
        while (n && out[n - 1] == ' ')
            out[--n] = 0;
    }
}

struct BatteryState {
    bool present = false;
    u64 design = 0, full = 0, now = 0, rate = 0, voltage = 0, design_voltage = 0, warning = 0, low = 0;
    u64 cycles = 0;
    u32 state = 0;     // _BST bit 0 discharging, bit 1 charging, bit 2 critical
    bool watts = true; // capacity unit: mWh (true) or mAh
    bool rechargeable = true;
    char model[24] = {}, serial[24] = {}, technology[12] = {}, manufacturer[24] = {};
    u64 info_time = 0, status_time = 0; // clock::now_ns() of the last _BIX / _BST
};
BatteryState g_battery_cache[MAX_BATTERIES];

constexpr u64 UNKNOWN = 0xFFFFFFFF;

// Reads (or reuses: power managers poll every second) a battery's state.
// Static information is re-read every 30 s, the status every 2 s.
const BatteryState& read_battery(int i) {
    BatteryState& s = g_battery_cache[i];
    uacpi_namespace_node* node = g_batteries[i];
    u64 now = clock::now_ns();
    bool fresh_info = s.info_time && now - s.info_time < 30'000'000'000ull;
    bool fresh_status = s.status_time && now - s.status_time < 2'000'000'000ull;
    if (fresh_info && fresh_status)
        return s;
    u64 sta = 0x1F;
    uacpi_eval_simple_integer(node, "_STA", &sta);
    bool was_present = s.present;
    s.present = sta & 0x10;
    if (!s.present)
        return s;
    if (!fresh_info || !was_present) {
        // _BIX (ACPI 4+): one revision field before _BIF's first five; strings further on.
        uacpi_object* info = nullptr;
        bool bix = uacpi_eval_simple(node, "_BIX", &info) == UACPI_STATUS_OK;
        if (!bix && uacpi_eval_simple(node, "_BIF", &info) != UACPI_STATUS_OK)
            info = nullptr;
        if (info) {
            uacpi_object_array a;
            if (uacpi_object_get_package(info, &a) == UACPI_STATUS_OK) {
                usize shift = bix ? 1 : 0, strings = bix ? 16 : 9;
                s.watts = package_int(a, 0 + shift) == 0;
                s.design = package_int(a, 1 + shift);
                s.full = package_int(a, 2 + shift);
                s.rechargeable = package_int(a, 3 + shift) != 0;
                s.design_voltage = package_int(a, 4 + shift);
                s.warning = package_int(a, 5 + shift);
                s.low = package_int(a, 6 + shift);
                s.cycles = bix ? package_int(a, 8) : 0;
                package_string(a, strings, s.model, sizeof(s.model));
                package_string(a, strings + 1, s.serial, sizeof(s.serial));
                package_string(a, strings + 2, s.technology, sizeof(s.technology));
                package_string(a, strings + 3, s.manufacturer, sizeof(s.manufacturer));
            }
            uacpi_object_unref(info);
        }
        if (s.design == UNKNOWN)
            s.design = 0;
        if (s.full == UNKNOWN || s.full == 0)
            s.full = s.design;
        if (s.design_voltage == UNKNOWN)
            s.design_voltage = 0;
        if (s.cycles == UNKNOWN)
            s.cycles = 0;
        s.info_time = now;
    }
    uacpi_object* st = nullptr;
    if (uacpi_eval_simple(node, "_BST", &st) == UACPI_STATUS_OK) {
        uacpi_object_array a;
        if (uacpi_object_get_package(st, &a) == UACPI_STATUS_OK) {
            s.state = static_cast<u32>(package_int(a, 0));
            s.rate = package_int(a, 1);
            s.now = package_int(a, 2);
            s.voltage = package_int(a, 3);
            if (s.rate == UNKNOWN)
                s.rate = 0;
            if (s.voltage == UNKNOWN || s.voltage == 0)
                s.voltage = s.design_voltage;
            if (s.now == UNKNOWN)
                s.now = 0;
            if (s.rate > 0x7FFFFFFF) // some firmware reports a negative rate while discharging
                s.rate = static_cast<u64>(-static_cast<i64>(static_cast<i32>(s.rate)));
        }
        uacpi_object_unref(st);
    }
    s.status_time = now;
    return s;
}

const char* status_text(const BatteryState& s) {
    if (s.state & 2) return "Charging";
    if (s.state & 1) return "Discharging";
    return s.full && s.now >= s.full * 95 / 100 ? "Full" : "Not charging";
}

u64 capacity_percent(const BatteryState& s) {
    u64 cap = s.full ? s.now * 100 / s.full : 0;
    return cap > 100 ? 100 : cap;
}

const char* capacity_level(const BatteryState& s) {
    if (s.state & 4 || (s.low && s.now <= s.low)) return "Critical";
    if (s.warning && s.now <= s.warning) return "Low";
    if (s.full && s.now >= s.full * 95 / 100) return "Full";
    return "Normal";
}

enum BatteryField {
    F_PRESENT, F_STATUS, F_CAPACITY, F_NOW, F_FULL, F_FULL_DESIGN, F_RATE, F_VOLTAGE, F_UEVENT, F_VOLTAGE_DESIGN,
    F_TECHNOLOGY, F_MANUFACTURER, F_MODEL, F_SERIAL, F_LEVEL, F_CYCLES, F_ALARM, F_COUNT
};

bool gen_battery(genfs::TextBuffer& out, const void* ctx) {
    int i = static_cast<int>(reinterpret_cast<uptr>(ctx) >> 8);
    int field = static_cast<int>(reinterpret_cast<uptr>(ctx) & 0xFF);
    if (i >= g_battery_count)
        return false;
    LockGuard guard(g_battery_lock);
    const BatteryState& s = read_battery(i);
    // Linux units: µWh/µW for mWh batteries, µAh/µA for mAh ones, µV.
    const char* cap = s.watts ? "ENERGY" : "CHARGE";
    const char* tech = !strncmp(s.technology, "LI", 2) || !strncmp(s.technology, "Li", 2) ? "Li-ion"
                       : !strncmp(s.technology, "NiMH", 4)                               ? "NiMH"
                                                                                         : "Unknown";
    switch (field) {
    case F_PRESENT: out.printf("%d\n", s.present ? 1 : 0); break;
    case F_STATUS: out.printf("%s\n", s.present ? status_text(s) : "Unknown"); break;
    case F_CAPACITY: out.printf("%lu\n", capacity_percent(s)); break;
    case F_NOW: out.printf("%lu\n", s.now * 1000); break;
    case F_FULL: out.printf("%lu\n", s.full * 1000); break;
    case F_FULL_DESIGN: out.printf("%lu\n", s.design * 1000); break;
    case F_RATE: out.printf("%lu\n", s.rate * 1000); break;
    case F_VOLTAGE: out.printf("%lu\n", s.voltage * 1000); break;
    case F_VOLTAGE_DESIGN: out.printf("%lu\n", s.design_voltage * 1000); break;
    case F_TECHNOLOGY: out.printf("%s\n", tech); break;
    case F_MANUFACTURER: out.printf("%s\n", s.manufacturer); break;
    case F_MODEL: out.printf("%s\n", s.model); break;
    case F_SERIAL: out.printf("%s\n", s.serial); break;
    case F_LEVEL: out.printf("%s\n", s.present ? capacity_level(s) : "Unknown"); break;
    case F_CYCLES: out.printf("%lu\n", s.cycles); break;
    case F_ALARM: out.printf("%lu\n", s.warning * 1000); break;
    case F_UEVENT:
        out.printf("POWER_SUPPLY_NAME=BAT%d\nPOWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_PRESENT=%d\n", i,
                   s.present ? 1 : 0);
        if (!s.present)
            break;
        out.printf("POWER_SUPPLY_STATUS=%s\nPOWER_SUPPLY_TECHNOLOGY=%s\nPOWER_SUPPLY_CYCLE_COUNT=%lu\n"
                   "POWER_SUPPLY_VOLTAGE_MIN_DESIGN=%lu\nPOWER_SUPPLY_VOLTAGE_NOW=%lu\nPOWER_SUPPLY_%s_NOW=%lu\n"
                   "POWER_SUPPLY_%s_FULL_DESIGN=%lu\nPOWER_SUPPLY_%s_FULL=%lu\nPOWER_SUPPLY_%s_NOW=%lu\n"
                   "POWER_SUPPLY_CAPACITY=%lu\nPOWER_SUPPLY_CAPACITY_LEVEL=%s\nPOWER_SUPPLY_MODEL_NAME=%s\n"
                   "POWER_SUPPLY_MANUFACTURER=%s\nPOWER_SUPPLY_SERIAL_NUMBER=%s\n",
                   status_text(s), tech, s.cycles, s.design_voltage * 1000, s.voltage * 1000,
                   s.watts ? "POWER" : "CURRENT", s.rate * 1000, cap, s.design * 1000, cap, s.full * 1000, cap,
                   s.now * 1000, capacity_percent(s), capacity_level(s), s.model, s.manufacturer, s.serial);
        break;
    default: return false;
    }
    return true;
}

bool ac_online() {
    u64 on = 1;
    LockGuard guard(g_battery_lock);
    uacpi_eval_simple_integer(g_ac, "_PSR", &on);
    return on != 0;
}

bool gen_ac(genfs::TextBuffer& out, const void*) {
    out.printf("%d\n", ac_online() ? 1 : 0);
    return true;
}

bool gen_ac_uevent(genfs::TextBuffer& out, const void*) {
    out.printf("POWER_SUPPLY_NAME=AC\nPOWER_SUPPLY_TYPE=Mains\nPOWER_SUPPLY_ONLINE=%d\n", ac_online() ? 1 : 0);
    return true;
}

bool gen_lid(genfs::TextBuffer& out, const void*) {
    u64 open = 1;
    uacpi_eval_simple_integer(g_lid, "_LID", &open);
    out.printf("state:      %s\n", open ? "open" : "closed");
    return true;
}

bool link_power_supply_subsystem(char* out, usize size, const void*) {
    snprintf(out, size, "../../../class/power_supply");
    return true;
}

uacpi_iteration_decision found_battery(void*, uacpi_namespace_node* node, uacpi_u32) {
    if (g_battery_count < MAX_BATTERIES)
        g_batteries[g_battery_count++] = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
uacpi_iteration_decision found_ac(void*, uacpi_namespace_node* node, uacpi_u32) {
    if (!g_ac)
        g_ac = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
uacpi_iteration_decision found_lid(void*, uacpi_namespace_node* node, uacpi_u32) {
    if (!g_lid)
        g_lid = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
uacpi_iteration_decision found_power_button(void*, uacpi_namespace_node* node, uacpi_u32) {
    uacpi_install_notify_handler(node, power_button_notify, nullptr);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// Battery/adapter events (0x80 status, 0x81 information changed): drop the cache.
uacpi_status battery_notify(uacpi_handle, uacpi_namespace_node*, uacpi_u64) {
    for (int i = 0; i < g_battery_count; i++)
        g_battery_cache[i].info_time = g_battery_cache[i].status_time = 0;
    return UACPI_STATUS_OK;
}

void publish_power_supplies() {
    for (int i = 0; i < g_battery_count; i++) {
        uacpi_install_notify_handler(g_batteries[i], battery_notify, nullptr);
        bool watts = true;
        {
            LockGuard guard(g_battery_lock);
            const BatteryState& s = read_battery(i);
            watts = !s.present || s.watts;
        }
        const char* names[F_COUNT] = {"present", "status", "capacity", watts ? "energy_now" : "charge_now",
                                      watts ? "energy_full" : "charge_full",
                                      watts ? "energy_full_design" : "charge_full_design",
                                      watts ? "power_now" : "current_now", "voltage_now", "uevent",
                                      "voltage_min_design", "technology", "manufacturer", "model_name",
                                      "serial_number", "capacity_level", "cycle_count", "alarm"};
        char dir[64], path[96];
        snprintf(dir, sizeof(dir), "/sys/class/power_supply/BAT%d", i);
        vfs::mkdir_p(dir);
        snprintf(path, sizeof(path), "%s/type", dir);
        genfs::static_file(path, "Battery\n");
        snprintf(path, sizeof(path), "%s/subsystem", dir);
        vfs::add_node(path, new genfs::GeneratedLink(link_power_supply_subsystem, nullptr));
        for (int f = 0; f < F_COUNT; f++) {
            snprintf(path, sizeof(path), "%s/%s", dir, names[f]);
            vfs::add_node(path, new genfs::GeneratedFile(gen_battery,
                                                         reinterpret_cast<const void*>(static_cast<uptr>((i << 8) | f))));
        }
    }
    if (g_ac) {
        uacpi_install_notify_handler(g_ac, battery_notify, nullptr);
        vfs::mkdir_p("/sys/class/power_supply/AC");
        genfs::static_file("/sys/class/power_supply/AC/type", "Mains\n");
        vfs::add_node("/sys/class/power_supply/AC/online", new genfs::GeneratedFile(gen_ac, nullptr));
        vfs::add_node("/sys/class/power_supply/AC/uevent", new genfs::GeneratedFile(gen_ac_uevent, nullptr));
        vfs::add_node("/sys/class/power_supply/AC/subsystem",
                      new genfs::GeneratedLink(link_power_supply_subsystem, nullptr));
    }
    if (g_lid) {
        vfs::mkdir_p("/proc/acpi/button/lid/LID0");
        vfs::add_node("/proc/acpi/button/lid/LID0/state", new genfs::GeneratedFile(gen_lid, nullptr));
    }
    for (int i = 0; i < g_battery_count; i++) {
        LockGuard guard(g_battery_lock);
        const BatteryState& s = read_battery(i);
        if (s.present)
            klog::info("ACPI: battery BAT%d: %s %s, %lu%%, %s (%lu/%lu m%sh, design %lu)", i, s.manufacturer,
                       s.model, capacity_percent(s), status_text(s), s.now, s.full, s.watts ? "W" : "A", s.design);
        else
            klog::info("ACPI: battery BAT%d: not inserted", i);
    }
}

} // namespace

void start_interpreter() {
    if (!rsdp_phys())
        return;
    if (strstr(boot::cmdline(), "acpi=noaml")) {
        klog::info("ACPI: AML interpreter disabled (acpi=noaml)");
        return;
    }
    uacpi_status r = uacpi_initialize(0);
    if (r == UACPI_STATUS_OK)
        r = uacpi_namespace_load();
    if (r == UACPI_STATUS_OK)
        ec::early_init(); // the firmware's _INI/_REG may already need the EC
    if (r == UACPI_STATUS_OK)
        r = uacpi_namespace_initialize();
    if (r == UACPI_STATUS_OK)
        ec::late_init();
    if (r != UACPI_STATUS_OK) {
        klog::warn("ACPI: interpreter not started (%s)", uacpi_status_to_string(r));
        return;
    }
    uacpi_finalize_gpe_initialization();
    rearm_interrupts();
    g_ready = true;
    uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_power_button, nullptr);
    uacpi_find_devices("PNP0C0C", found_power_button, nullptr);
    uacpi_find_devices("PNP0C0A", found_battery, nullptr);
    uacpi_find_devices("ACPI0003", found_ac, nullptr);
    uacpi_find_devices("PNP0C0D", found_lid, nullptr);
    publish_power_supplies();
    // What power managers (TDE's tdepowersave) look for to know ACPI is
    // there; no sleep states are offered until resume works.
    vfs::mkdir_p("/proc/acpi");
    vfs::mkdir_p("/sys/firmware/acpi");
    vfs::mkdir_p("/sys/power");
    genfs::static_file("/sys/power/state", "\n");
    klog::ok("ACPI: AML interpreter running (uACPI); %d battery(ies), AC adapter %s, lid %s, power button on",
             g_battery_count, g_ac ? "yes" : "no", g_lid ? "yes" : "no");
}

bool interpreter_ready() { return g_ready; }

void enter_s5() {
    uacpi_status r = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5); // runs AML: may sleep
    if (r != UACPI_STATUS_OK) {
        klog::warn("ACPI: preparing for power off failed (%s)", uacpi_status_to_string(r));
        return;
    }
    arch::cli();
    r = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    klog::warn("ACPI: power off through the interpreter failed (%s)", uacpi_status_to_string(r));
}

} // namespace acpi
