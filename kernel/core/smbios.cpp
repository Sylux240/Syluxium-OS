// SMBIOS reader: the System (type 1) and Baseboard (type 2) structures'
// manufacturer and product strings.
#include <boot/limine_requests.hpp>
#include <core/smbios.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/vmalloc.hpp>

namespace smbios {

namespace {

char g_name[96];
bool g_done = false;

// The n-th (1-based) string after a structure's formatted area.
const char* string_at(const u8* s, u8 index, const u8* end) {
    if (!index)
        return "";
    const char* p = reinterpret_cast<const char*>(s + s[1]);
    for (u8 i = 1; i < index && reinterpret_cast<const u8*>(p) < end; i++)
        p += strlen(p) + 1;
    return reinterpret_cast<const u8*>(p) < end ? p : "";
}

bool useful(const char* v) {
    if (!v || !*v)
        return false;
    static const char* const junk[] = {"To Be Filled By O.E.M.", "To be filled by O.E.M.", "Default string",
                                       "System Product Name", "System manufacturer", "Not Applicable", "None",
                                       "O.E.M.", "Unknown"};
    for (const char* j : junk)
        if (strcmp(v, j) == 0)
            return false;
    return true;
}

void append(char* out, usize size, const char* word) {
    usize len = strlen(out);
    // Skip the manufacturer when the product already starts with it ("Dell Inc. Dell ...").
    snprintf(out + len, size - len, "%s%s", len ? " " : "", word);
}

void read_table(u64 table_phys, u32 table_len) {
    if (!table_phys || !table_len || table_len > 1024 * 1024)
        return;
    auto* base = static_cast<const u8*>(vmalloc::map_physical(table_phys, table_len, 0));
    if (!base)
        return;
    const u8* end = base + table_len;
    char sys_vendor[48] = {}, sys_product[48] = {}, board_vendor[48] = {}, board_product[48] = {};
    for (const u8* s = base; s + 4 <= end && s[1] >= 4;) {
        u8 type = s[0];
        if (type == 127)
            break;
        if (type == 1 && s[1] >= 8) {
            snprintf(sys_vendor, sizeof(sys_vendor), "%s", string_at(s, s[4], end));
            snprintf(sys_product, sizeof(sys_product), "%s", string_at(s, s[5], end));
            // Lenovo keeps the model name ("ThinkPad P53s") in Version.
            const char* version = string_at(s, s[6], end);
            if (strncmp(sys_vendor, "LENOVO", 6) == 0 && useful(version))
                snprintf(sys_product, sizeof(sys_product), "%s", version);
        } else if (type == 2 && s[1] >= 6) {
            snprintf(board_vendor, sizeof(board_vendor), "%s", string_at(s, s[4], end));
            snprintf(board_product, sizeof(board_product), "%s", string_at(s, s[5], end));
        }
        // Next structure: after the formatted area and its double-NUL-terminated strings.
        const u8* p = s + s[1];
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0))
            p++;
        s = p + 2;
    }
    vmalloc::unmap_physical(const_cast<u8*>(base));
    const char* vendor = useful(sys_product) ? sys_vendor : board_vendor;
    const char* product = useful(sys_product) ? sys_product : board_product;
    if (useful(vendor) && strncmp(product, vendor, strlen(vendor)) != 0)
        append(g_name, sizeof(g_name), vendor);
    if (useful(product))
        append(g_name, sizeof(g_name), product);
}

} // namespace

const char* machine_name() {
    if (g_done)
        return g_name;
    g_done = true;
    u64 e32, e64;
    boot::smbios_entries(&e32, &e64);
    if (e64) { // SMBIOS 3: "_SM3_", table address at 0x10, maximum size at 0x0C
        auto* ep = static_cast<const u8*>(vmalloc::map_physical(e64, 32, 0));
        if (ep && memcmp(ep, "_SM3_", 5) == 0) {
            u64 addr;
            u32 len;
            memcpy(&addr, ep + 0x10, 8);
            memcpy(&len, ep + 0x0C, 4);
            vmalloc::unmap_physical(const_cast<u8*>(ep));
            read_table(addr, len);
        } else if (ep) {
            vmalloc::unmap_physical(const_cast<u8*>(ep));
        }
    }
    if (!g_name[0] && e32) { // SMBIOS 2: "_SM_", table length at 0x16, address at 0x18
        auto* ep = static_cast<const u8*>(vmalloc::map_physical(e32, 32, 0));
        if (ep && memcmp(ep, "_SM_", 4) == 0) {
            u16 len;
            u32 addr;
            memcpy(&len, ep + 0x16, 2);
            memcpy(&addr, ep + 0x18, 4);
            vmalloc::unmap_physical(const_cast<u8*>(ep));
            read_table(addr, len);
        } else if (ep) {
            vmalloc::unmap_physical(const_cast<u8*>(ep));
        }
    }
    return g_name;
}

} // namespace smbios
