// ACPI devices through the AML interpreter (uACPI): the power button, the
// battery, the AC adapter and the lid. Batteries and adapters appear in
// /sys/class/power_supply like on Linux (TDE's battery monitor reads them).

#include <core/bootlog.hpp>
#include <boot/limine_requests.hpp>
#include <acpi/acpi.hpp>
#include <acpi/devices.hpp>
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

#include <uacpi/event.h>
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

constexpr int MAX_BATTERIES = 4;
uacpi_namespace_node* g_batteries[MAX_BATTERIES];
int g_battery_count = 0;
uacpi_namespace_node* g_ac = nullptr;
uacpi_namespace_node* g_lid = nullptr;

u64 package_int(uacpi_object_array& a, usize i) {
    u64 v = 0;
    if (i < a.count)
        uacpi_object_get_integer(a.objects[i], &v);
    return v;
}

struct BatteryState {
    bool present = false;
    u64 design = 0, full = 0, now = 0, rate = 0, voltage = 0;
    u32 state = 0; // _BST bit 0 discharging, bit 1 charging
    bool watts = true; // capacity unit: mWh (true) or mAh
};

bool read_battery(uacpi_namespace_node* node, BatteryState* s) {
    u64 sta = 0x1F;
    uacpi_eval_simple_integer(node, "_STA", &sta);
    s->present = sta & 0x10;
    if (!s->present)
        return true;
    // _BIX (ACPI 4+) has the same fields one later than _BIF.
    uacpi_object* info = nullptr;
    usize shift = 1;
    if (uacpi_eval_simple(node, "_BIX", &info) != UACPI_STATUS_OK) {
        shift = 0;
        if (uacpi_eval_simple(node, "_BIF", &info) != UACPI_STATUS_OK)
            info = nullptr;
    }
    if (info) {
        uacpi_object_array a;
        if (uacpi_object_get_package(info, &a) == UACPI_STATUS_OK) {
            s->watts = package_int(a, 0 + shift) == 0;
            s->design = package_int(a, 1 + shift);
            s->full = package_int(a, 2 + shift);
            s->voltage = package_int(a, 4 + shift);
        }
        uacpi_object_unref(info);
    }
    uacpi_object* st = nullptr;
    if (uacpi_eval_simple(node, "_BST", &st) == UACPI_STATUS_OK) {
        uacpi_object_array a;
        if (uacpi_object_get_package(st, &a) == UACPI_STATUS_OK) {
            s->state = static_cast<u32>(package_int(a, 0));
            s->rate = package_int(a, 1);
            s->now = package_int(a, 2);
        }
        uacpi_object_unref(st);
    }
    if (s->full == 0xFFFFFFFF || s->full == 0)
        s->full = s->design;
    return true;
}

const char* status_text(const BatteryState& s) {
    if (s.state & 2) return "Charging";
    if (s.state & 1) return "Discharging";
    return s.now >= s.full * 98 / 100 ? "Full" : "Not charging";
}

bool gen_battery(genfs::TextBuffer& out, const void* ctx) {
    int i = static_cast<int>(reinterpret_cast<uptr>(ctx) >> 8);
    int field = static_cast<int>(reinterpret_cast<uptr>(ctx) & 0xFF);
    BatteryState s;
    if (i >= g_battery_count || !read_battery(g_batteries[i], &s))
        return false;
    u64 cap = s.full ? s.now * 100 / s.full : 0;
    if (cap > 100)
        cap = 100;
    const char* unit = s.watts ? "energy" : "charge";
    switch (field) {
    case 0: out.printf("%d\n", s.present ? 1 : 0); break;
    case 1: out.printf("%s\n", status_text(s)); break;
    case 2: out.printf("%lu\n", cap); break;
    case 3: out.printf("%lu\n", s.now * 1000); break;  // µWh / µAh
    case 4: out.printf("%lu\n", s.full * 1000); break;
    case 5: out.printf("%lu\n", s.design * 1000); break;
    case 6: out.printf("%lu\n", s.rate * 1000); break; // µW / µA
    case 7: out.printf("%lu\n", s.voltage * 1000); break;
    case 8:
        out.printf("POWER_SUPPLY_NAME=BAT%d\nPOWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_PRESENT=%d\n"
                   "POWER_SUPPLY_STATUS=%s\nPOWER_SUPPLY_CAPACITY=%lu\nPOWER_SUPPLY_%s_NOW=%lu\n"
                   "POWER_SUPPLY_%s_FULL=%lu\nPOWER_SUPPLY_%s_FULL_DESIGN=%lu\n",
                   i, s.present ? 1 : 0, status_text(s), cap, s.watts ? "ENERGY" : "CHARGE", s.now * 1000,
                   s.watts ? "ENERGY" : "CHARGE", s.full * 1000, s.watts ? "ENERGY" : "CHARGE", s.design * 1000);
        break;
    }
    (void)unit;
    return true;
}

bool gen_ac(genfs::TextBuffer& out, const void*) {
    u64 on = 1;
    uacpi_eval_simple_integer(g_ac, "_PSR", &on);
    out.printf("%lu\n", on ? 1ul : 0ul);
    return true;
}

bool gen_lid(genfs::TextBuffer& out, const void*) {
    u64 open = 1;
    uacpi_eval_simple_integer(g_lid, "_LID", &open);
    out.printf("state:      %s\n", open ? "open" : "closed");
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

void publish_power_supplies() {
    static const char* const fields[] = {"present", "status", "capacity", "energy_now", "energy_full",
                                         "energy_full_design", "power_now", "voltage_now", "uevent"};
    for (int i = 0; i < g_battery_count; i++) {
        char dir[64], path[96];
        snprintf(dir, sizeof(dir), "/sys/class/power_supply/BAT%d", i);
        vfs::mkdir_p(dir);
        snprintf(path, sizeof(path), "%s/type", dir);
        genfs::static_file(path, "Battery\n");
        for (int f = 0; f < 9; f++) {
            snprintf(path, sizeof(path), "%s/%s", dir, fields[f]);
            vfs::add_node(path, new genfs::GeneratedFile(gen_battery,
                                                         reinterpret_cast<const void*>(static_cast<uptr>((i << 8) | f))));
        }
    }
    if (g_ac) {
        vfs::mkdir_p("/sys/class/power_supply/AC");
        genfs::static_file("/sys/class/power_supply/AC/type", "Mains\n");
        vfs::add_node("/sys/class/power_supply/AC/online", new genfs::GeneratedFile(gen_ac, nullptr));
    }
    if (g_lid) {
        vfs::mkdir_p("/proc/acpi/button/lid/LID0");
        vfs::add_node("/proc/acpi/button/lid/LID0/state", new genfs::GeneratedFile(gen_lid, nullptr));
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
        r = uacpi_namespace_initialize();
    if (r != UACPI_STATUS_OK) {
        klog::warn("ACPI: interpreter not started (%s)", uacpi_status_to_string(r));
        return;
    }
    uacpi_finalize_gpe_initialization();
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

} // namespace acpi
