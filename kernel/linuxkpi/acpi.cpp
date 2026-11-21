// LinuxKPI: ACPI for Linux drivers, from uACPI.
//  - acpi_get_table(): GPU drivers read the video BIOS of discrete cards from
//    the VFCT table on UEFI machines
//  - ACPI companions of PCI devices and method evaluation: on Optimus
//    laptops the NVIDIA chip's video BIOS only comes from its _ROM method
#include <acpi/acpi.hpp>
#include <acpi/devices.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <lib/types.hpp>
#include <mm/heap.hpp>
#include <uacpi/namespace.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

// The table with signature `sig`, instance 1, 2, ... (kept mapped), or null.
extern "C" void* lkpi_acpi_find_table(const char* sig, unsigned instance) {
    // GPU drivers probe before uACPI starts (AMD APUs' video BIOS is only in
    // VFCT): the tables the kernel found at boot then.
    if (!acpi::interpreter_ready())
        return const_cast<acpi::SdtHeader*>(acpi::find_table(sig, instance ? instance - 1 : 0));
    uacpi_table tbl;
    if (uacpi_table_find_by_signature(sig, &tbl) != UACPI_STATUS_OK)
        return nullptr;
    for (unsigned i = 1; i < instance; i++) {
        uacpi_table_ref(&tbl); // find_next drops one reference
        if (uacpi_table_find_next_with_same_signature(&tbl) != UACPI_STATUS_OK)
            return nullptr;
    }
    return tbl.ptr;
}

namespace {

struct RootSearch {
    u64 bus;
    uacpi_namespace_node* found;
};

uacpi_iteration_decision root_cb(void* user, uacpi_namespace_node* node, uacpi_u32) {
    auto* rs = static_cast<RootSearch*>(user);
    u64 bbn = 0;
    uacpi_eval_simple_integer(node, "_BBN", &bbn); // absent: bus 0
    if (bbn == rs->bus) {
        rs->found = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

struct ChildSearch {
    u64 adr;
    uacpi_namespace_node* found;
};

uacpi_iteration_decision child_cb(void* user, uacpi_namespace_node* node, uacpi_u32 depth) {
    auto* cs = static_cast<ChildSearch*>(user);
    u64 adr;
    if (uacpi_eval_simple_integer(node, "_ADR", &adr) == UACPI_STATUS_OK && adr == cs->adr) {
        cs->found = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    (void)depth;
    return UACPI_ITERATION_DECISION_NEXT_PEER; // direct children only
}

} // namespace

// The PCI host bridge (PNP0A08 / PNP0A03) for `bus`, or null (also while
// the AML interpreter isn't running).
// ACPI companion lookups that came before the AML interpreter ran (GPU
// probes run first): a probe that failed after one is retried later.
unsigned long g_acpi_early_lookups = 0;

extern "C" void* lkpi_acpi_pci_root(unsigned bus) {
    if (!acpi::interpreter_ready()) {
        __atomic_fetch_add(&g_acpi_early_lookups, 1, __ATOMIC_RELAXED);
        return nullptr;
    }
    static const uacpi_char* const hids[] = {"PNP0A08", "PNP0A03", nullptr};
    RootSearch rs{bus, nullptr};
    uacpi_find_devices_at(uacpi_namespace_root(), hids, root_cb, &rs);
    return rs.found;
}

// The child of `parent` whose _ADR is `adr` (PCI: device << 16 | function).
extern "C" void* lkpi_acpi_child_by_adr(void* parent, u64 adr) {
    if (!parent)
        return nullptr;
    ChildSearch cs{adr, nullptr};
    uacpi_namespace_for_each_child_simple(static_cast<uacpi_namespace_node*>(parent), child_cb, &cs);
    return cs.found;
}

extern "C" void* lkpi_acpi_find(void* parent, const char* path) {
    if (!parent || !acpi::interpreter_ready())
        return nullptr;
    uacpi_namespace_node* out = nullptr;
    if (uacpi_namespace_node_find(static_cast<uacpi_namespace_node*>(parent), path, &out) != UACPI_STATUS_OK)
        return nullptr;
    return out;
}

// Evaluate `node` (a method object) with integer arguments; a buffer result
// is copied into a new kmalloc() block (*data, *len). 0 or -1.
extern "C" int lkpi_acpi_eval_buffer(void* node, const u64* ints, unsigned nints, void** data, u32* len) {
    if (!node || nints > 7)
        return -1;
    uacpi_object* argv[7] = {};
    for (unsigned i = 0; i < nints; i++)
        argv[i] = uacpi_object_create_integer(ints[i]);
    uacpi_object_array args{argv, nints};
    uacpi_object* ret = nullptr;
    uacpi_status st = uacpi_eval(static_cast<uacpi_namespace_node*>(node), nullptr, nints ? &args : nullptr, &ret);
    for (unsigned i = 0; i < nints; i++)
        uacpi_object_unref(argv[i]);
    if (st != UACPI_STATUS_OK || !ret)
        return -1;
    uacpi_data_view view;
    int r = -1;
    if (uacpi_object_get_buffer(ret, &view) == UACPI_STATUS_OK) {
        void* copy = kmalloc(view.length ? view.length : 1);
        if (copy) {
            memcpy(copy, view.const_data, view.length);
            *data = copy;
            *len = static_cast<u32>(view.length);
            r = 0;
        }
    }
    uacpi_object_unref(ret);
    return r;
}

extern "C" bool lkpi_acpi_has_method(void* node, const char* name) { return lkpi_acpi_find(node, name) != nullptr; }

// The absolute ACPI path of `node` ("\\_SB_.PCI0.GFX0") into buf.
extern "C" void lkpi_acpi_path(void* node, char* buf, unsigned size) {
    buf[0] = 0;
    if (!node || !size)
        return;
    const uacpi_char* path = uacpi_namespace_node_generate_absolute_path(static_cast<uacpi_namespace_node*>(node));
    if (!path)
        return;
    snprintf(buf, size, "%s", path);
    uacpi_free_absolute_path(path);
}
