#include <core/log.hpp>
#include <fs/initramfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>

namespace initramfs {

namespace {

constexpr usize HEADER_SIZE = 110;

// Header fields are 8 ASCII hex digits each.
u32 hex_field(const char* p) {
    u32 v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    }
    return v;
}

usize align4(usize v) { return (v + 3) & ~usize(3); }

} // namespace

long unpack(const void* archive, usize size, Stats* stats) {
    *stats = {};
    auto* base = static_cast<const char*>(archive);
    usize pos = 0;

    while (pos + HEADER_SIZE <= size) {
        const char* h = base + pos;
        if (memcmp(h, "070701", 6) != 0 && memcmp(h, "070702", 6) != 0) {
            klog::error("initramfs: bad cpio magic at offset %zu", pos);
            return -err::INVAL;
        }
        u32 mode = hex_field(h + 14);
        u32 file_size = hex_field(h + 54);
        u32 name_size = hex_field(h + 94);

        usize name_pos = pos + HEADER_SIZE;
        usize data_pos = align4(name_pos + name_size);
        usize next = align4(data_pos + file_size);
        if (name_size == 0 || data_pos + file_size > size)
            return -err::INVAL;

        const char* name = base + name_pos;
        if (strcmp(name, "TRAILER!!!") == 0)
            break;

        // Normalize "./bin/x" or "bin/x" into "/bin/x".
        char path[512];
        const char* n = name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        else if (n[0] == '.' && n[1] == 0) n += 1;
        while (*n == '/') n++;
        snprintf(path, sizeof(path), "/%s", n);

        const char* data = base + data_pos;
        u32 perms = mode & 07777;
        long r = 0;

        switch (mode & vfs::S_IFMT) {
        case vfs::S_IFDIR:
            r = vfs::mkdir_p(path, perms);
            stats->dirs++;
            break;
        case vfs::S_IFREG: {
            // Make sure the parent exists (archives don't always list directories first).
            char parent[512];
            snprintf(parent, sizeof(parent), "%s", path);
            char* slash = nullptr;
            for (char* q = parent; *q; q++)
                if (*q == '/')
                    slash = q;
            if (slash && slash != parent) {
                *slash = 0;
                vfs::mkdir_p(parent);
            }
            r = vfs::create_file(path, data, file_size, perms);
            stats->files++;
            stats->bytes += file_size;
            break;
        }
        case vfs::S_IFLNK: {
            char target[256];
            usize len = min<usize>(file_size, sizeof(target) - 1);
            memcpy(target, data, len);
            target[len] = 0;
            vfs::Inode* parent;
            const char* leaf;
            usize leaf_len;
            r = vfs::resolve_parent(path, nullptr, &parent, &leaf, &leaf_len);
            if (r == 0)
                r = parent->create(leaf, leaf_len, vfs::S_IFLNK | 0777, target, nullptr);
            stats->symlinks++;
            break;
        }
        default:
            // Device nodes, FIFOs: created by the kernel itself.
            break;
        }
        if (r < 0 && r != -err::EXIST)
            klog::warn("initramfs: could not create %s (error %ld)", path, -r);
        pos = next;
    }
    return 0;
}

} // namespace initramfs
