// ACPI Embedded Controller (EC): the laptop microcontroller behind the
// battery, AC adapter, lid, fans, hotkeys and thermal sensors. The firmware's
// AML reads and writes its 256-byte register space ("EmbeddedControl"
// operation regions) through two I/O ports, and the EC raises a GPE when it
// has an event, which the OS fetches with a query command and handles by
// running the matching _Qxx method. Without this, laptops' AML fails and
// unacknowledged EC events can keep the SCI firing.

#include <acpi/ec.hpp>
#include <arch/x86_64/cpu.hpp>
#include <core/log.hpp>
#include <lib/format.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace acpi::ec {

namespace {

constexpr u8 ST_OBF = 0x01, ST_IBF = 0x02, ST_SCI_EVT = 0x20;
constexpr u8 CMD_READ = 0x80, CMD_WRITE = 0x81, CMD_QUERY = 0x84;

u16 g_cmd_port = 0, g_data_port = 0;
bool g_present = false;
bool g_handler_installed = false;
bool g_use_glk = false;
bool g_query_pending = false;
uacpi_namespace_node* g_node = nullptr;
i32 g_gpe = -1;
sched::Mutex g_lock;
u32 g_timeouts = 0;

u8 status() { return arch::inb(g_cmd_port); }

bool wait_for(u8 mask, bool set) {
    u64 deadline = clock::now_ns() + 100'000'000;
    while (((status() & mask) != 0) != set) {
        if (clock::now_ns() > deadline) {
            if (g_timeouts++ < 5)
                klog::warn("EC: timed out waiting for %s (status %#x)", mask == ST_IBF ? "input buffer" : "output",
                           status());
            return false;
        }
        arch::pause();
    }
    return true;
}

bool send_cmd(u8 cmd) {
    if (!wait_for(ST_IBF, false))
        return false;
    arch::outb(g_cmd_port, cmd);
    return true;
}

bool write_data(u8 v) {
    if (!wait_for(ST_IBF, false))
        return false;
    arch::outb(g_data_port, v);
    return true;
}

bool read_data(u8* v) {
    if (!wait_for(ST_OBF, true))
        return false;
    *v = arch::inb(g_data_port);
    return true;
}

// Transactions are serialized; the firmware may require the ACPI global lock.
struct Transaction {
    u32 seq = 0;
    bool glk = false;
    Transaction() {
        g_lock.lock();
        if (g_use_glk)
            glk = uacpi_acquire_global_lock(0xFFFF, &seq) == UACPI_STATUS_OK;
    }
    ~Transaction() {
        if (glk)
            uacpi_release_global_lock(seq);
        g_lock.unlock();
    }
};

bool ec_read(u8 addr, u8* v) {
    Transaction t;
    return send_cmd(CMD_READ) && write_data(addr) && read_data(v);
}

bool ec_write(u8 addr, u8 v) {
    Transaction t;
    return send_cmd(CMD_WRITE) && write_data(addr) && write_data(v) && wait_for(ST_IBF, false);
}

uacpi_status region_handler(uacpi_region_op op, uacpi_handle op_data) {
    if (op == UACPI_REGION_OP_ATTACH || op == UACPI_REGION_OP_DETACH)
        return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE)
        return UACPI_STATUS_INVALID_ARGUMENT;
    auto* rw = static_cast<uacpi_region_rw_data*>(op_data);
    u64 offset = rw->offset;
    u32 width = rw->byte_width;
    if (offset + width > 256)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (op == UACPI_REGION_OP_READ) {
        u64 value = 0;
        for (u32 i = 0; i < width; i++) {
            u8 b;
            if (!ec_read(static_cast<u8>(offset + i), &b))
                return UACPI_STATUS_HARDWARE_TIMEOUT;
            value |= static_cast<u64>(b) << (8 * i);
        }
        rw->value = value;
    } else {
        for (u32 i = 0; i < width; i++)
            if (!ec_write(static_cast<u8>(offset + i), static_cast<u8>(rw->value >> (8 * i))))
                return UACPI_STATUS_HARDWARE_TIMEOUT;
    }
    return UACPI_STATUS_OK;
}

// ---- events: query the EC, run _Qxx ----

void query_work(uacpi_handle) {
    for (int round = 0; round < 32; round++) {
        u8 q = 0;
        {
            Transaction t;
            if (!send_cmd(CMD_QUERY) || !read_data(&q))
                q = 0;
        }
        if (q == 0)
            break; // no more events
        char method[8];
        snprintf(method, sizeof(method), "_Q%02X", q);
        uacpi_status r = uacpi_execute(g_node, method, nullptr);
        if (r != UACPI_STATUS_OK && r != UACPI_STATUS_NOT_FOUND)
            klog::warn("EC: %s failed (%s)", method, uacpi_status_to_string(r));
    }
    __atomic_store_n(&g_query_pending, false, __ATOMIC_RELEASE);
}

uacpi_interrupt_ret gpe_handler(uacpi_handle, uacpi_namespace_node*, uacpi_u16) {
    if ((status() & ST_SCI_EVT) && !__atomic_exchange_n(&g_query_pending, true, __ATOMIC_ACQ_REL))
        uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, nullptr, query_work, nullptr);
    return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
}

// ---- finding the EC ----

struct PortSearch {
    u16 ports[2];
    int count;
};

uacpi_iteration_decision collect_port(void* user, uacpi_resource* res) {
    auto* s = static_cast<PortSearch*>(user);
    u16 port = 0;
    if (res->type == UACPI_RESOURCE_TYPE_IO)
        port = res->io.minimum;
    else if (res->type == UACPI_RESOURCE_TYPE_FIXED_IO)
        port = res->fixed_io.address;
    else
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (s->count < 2)
        s->ports[s->count++] = port;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

uacpi_iteration_decision found_ec_device(void* user, uacpi_namespace_node* node, uacpi_u32) {
    *static_cast<uacpi_namespace_node**>(user) = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

void install_handler(uacpi_namespace_node* at) {
    uacpi_status r = uacpi_install_address_space_handler(at, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                                                         region_handler, nullptr);
    if (r != UACPI_STATUS_OK) {
        klog::warn("EC: could not install the operation region handler (%s)", uacpi_status_to_string(r));
        return;
    }
    g_handler_installed = true;
}

} // namespace

void early_init() {
    uacpi_table tbl;
    if (uacpi_table_find_by_signature("ECDT", &tbl) != UACPI_STATUS_OK)
        return;
    auto* ecdt = static_cast<acpi_ecdt*>(tbl.ptr);
    if (ecdt->ec_control.address_space_id == 1 && ecdt->ec_data.address_space_id == 1 &&
        ecdt->ec_control.address && ecdt->ec_data.address) {
        g_cmd_port = static_cast<u16>(ecdt->ec_control.address);
        g_data_port = static_cast<u16>(ecdt->ec_data.address);
        g_gpe = ecdt->gpe_bit;
        g_present = true;
        uacpi_namespace_node* node = nullptr;
        if (ecdt->hdr.length > sizeof(acpi_ecdt) && ecdt->ec_id[0])
            uacpi_namespace_node_find(nullptr, ecdt->ec_id, &node);
        g_node = node;
        // Early (before _INI/_REG run) so the firmware's start-up code can use it.
        install_handler(node ? node : uacpi_namespace_root());
    }
    uacpi_table_unref(&tbl);
}

void late_init() {
    // The EC device itself: its node (for _Qxx), _GPE, _GLK, and the ports
    // from _CRS when there was no ECDT.
    uacpi_namespace_node* dev = nullptr;
    uacpi_find_devices("PNP0C09", found_ec_device, &dev);
    if (!g_node)
        g_node = dev;
    if (!g_present && dev) {
        PortSearch s{{0, 0}, 0};
        uacpi_for_each_device_resource(dev, "_CRS", collect_port, &s);
        if (s.count == 2) { // data port first, then command/status
            g_data_port = s.ports[0];
            g_cmd_port = s.ports[1];
            g_present = true;
            install_handler(dev);
        }
    }
    if (!g_present || !g_node)
        return;
    u64 v = 0;
    if (uacpi_eval_simple_integer(g_node, "_GPE", &v) == UACPI_STATUS_OK)
        g_gpe = static_cast<i32>(v);
    if (uacpi_eval_simple_integer(g_node, "_GLK", &v) == UACPI_STATUS_OK)
        g_use_glk = v != 0;
    bool events = false;
    if (g_gpe >= 0 && uacpi_install_gpe_handler(nullptr, static_cast<u16>(g_gpe), UACPI_GPE_TRIGGERING_EDGE,
                                                gpe_handler, nullptr) == UACPI_STATUS_OK) {
        uacpi_enable_gpe(nullptr, static_cast<u16>(g_gpe));
        events = true;
        // Events that arrived before the handler: fetch them now.
        if (status() & ST_SCI_EVT) {
            __atomic_store_n(&g_query_pending, true, __ATOMIC_RELEASE);
            uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, nullptr, query_work, nullptr);
        }
    }
    klog::ok("ACPI: embedded controller at ports %#x/%#x%s, events on GPE %d%s", g_data_port, g_cmd_port,
             g_handler_installed ? "" : " (no region handler!)", g_gpe, events ? "" : " (not handled)");
}

bool present() { return g_present; }

} // namespace acpi::ec
