// LinuxKPI: ACPI tables for Linux drivers (acpi_get_table), from uACPI.
// GPU drivers read the video BIOS of discrete cards from the VFCT table on
// UEFI machines, where the card's expansion ROM often isn't readable.
#include <lib/types.hpp>
#include <uacpi/tables.h>

// The table with signature `sig`, instance 1, 2, ... (kept mapped), or null.
extern "C" void* lkpi_acpi_find_table(const char* sig, unsigned instance) {
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
