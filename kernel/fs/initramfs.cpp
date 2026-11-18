#include <core/log.hpp>
#include <fs/initramfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/inflate.hpp>
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

// A cpio stream unpacker: fed the archive in pieces of any size (straight
// from memory, or as the decompressor produces it).
class Unpacker {
public:
    explicit Unpacker(Stats* stats) : m_stats(stats) { *stats = {}; }
    bool done() const { return m_state == State::Done; }
    bool failed() const { return m_failed; }

    bool feed(const u8* data, usize len) {
        while (len && !m_failed && m_state != State::Done) {
            usize used = step(data, len);
            data += used;
            len -= used;
            m_offset += used;
        }
        return !m_failed;
    }

private:
    enum class State { Header, Name, NamePad, Data, DataPad, Done };
    State m_state = State::Header;
    Stats* m_stats;
    bool m_failed = false;
    u64 m_offset = 0;      // bytes consumed so far (alignment is relative to the archive start)
    char m_header[HEADER_SIZE];
    usize m_have = 0;      // bytes collected for the current field
    u32 m_mode = 0, m_file_size = 0, m_name_size = 0;
    char m_name[512];
    char m_path[512];
    vfs::Inode* m_file = nullptr; // regular file being filled
    char m_link[256];

    usize pad_bytes() const { return static_cast<usize>(align4(m_offset) - m_offset); }

    usize step(const u8* data, usize len) {
        switch (m_state) {
        case State::Header: {
            usize n = min<usize>(len, HEADER_SIZE - m_have);
            memcpy(m_header + m_have, data, n);
            m_have += n;
            if (m_have == HEADER_SIZE) {
                if (memcmp(m_header, "070701", 6) != 0 && memcmp(m_header, "070702", 6) != 0) {
                    klog::error("initramfs: bad cpio magic at offset %lu", m_offset + n - HEADER_SIZE);
                    m_failed = true;
                    return n;
                }
                m_mode = hex_field(m_header + 14);
                m_file_size = hex_field(m_header + 54);
                m_name_size = hex_field(m_header + 94);
                if (m_name_size == 0 || m_name_size > sizeof(m_name)) {
                    m_failed = true;
                    return n;
                }
                m_have = 0;
                m_state = State::Name;
            }
            return n;
        }
        case State::Name: {
            usize n = min<usize>(len, m_name_size - m_have);
            memcpy(m_name + m_have, data, n);
            m_have += n;
            if (m_have == m_name_size) {
                m_name[m_name_size - 1] = 0;
                m_have = 0;
                m_state = State::NamePad;
            }
            return n;
        }
        case State::NamePad: {
            usize pad = pad_bytes();
            if (pad) {
                usize n = min(len, pad);
                return n;
            }
            if (strcmp(m_name, "TRAILER!!!") == 0) {
                m_state = State::Done;
                return 0;
            }
            begin_entry();
            m_state = State::Data;
            return 0;
        }
        case State::Data: {
            if (m_have == m_file_size) {
                end_entry();
                m_have = 0;
                m_state = State::DataPad;
                return 0;
            }
            usize n = min<usize>(len, m_file_size - m_have);
            if (m_file) {
                if (m_file->write(m_have, data, n) < 0)
                    klog::warn("initramfs: could not write %s", m_path);
            } else if ((m_mode & vfs::S_IFMT) == vfs::S_IFLNK && m_have < sizeof(m_link) - 1) {
                usize k = min<usize>(n, sizeof(m_link) - 1 - m_have);
                memcpy(m_link + m_have, data, k);
            }
            m_have += n;
            return n;
        }
        case State::DataPad: {
            usize pad = pad_bytes();
            if (pad)
                return min(len, pad);
            m_state = State::Header;
            return 0;
        }
        case State::Done:
            return len;
        }
        return len;
    }

    void begin_entry() {
        // Normalize "./bin/x" or "bin/x" into "/bin/x".
        const char* n = m_name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        else if (n[0] == '.' && n[1] == 0) n += 1;
        while (*n == '/') n++;
        snprintf(m_path, sizeof(m_path), "/%s", n);
        m_file = nullptr;
        u32 perms = m_mode & 07777;
        long r = 0;
        switch (m_mode & vfs::S_IFMT) {
        case vfs::S_IFDIR:
            r = vfs::mkdir_p(m_path, perms);
            m_stats->dirs++;
            break;
        case vfs::S_IFREG: {
            // Make sure the parent exists (archives don't always list directories first).
            char parent[512];
            snprintf(parent, sizeof(parent), "%s", m_path);
            char* slash = nullptr;
            for (char* q = parent; *q; q++)
                if (*q == '/')
                    slash = q;
            if (slash && slash != parent) {
                *slash = 0;
                vfs::mkdir_p(parent);
            }
            vfs::Inode* dir;
            const char* leaf;
            usize leaf_len;
            r = vfs::resolve_parent(m_path, nullptr, &dir, &leaf, &leaf_len);
            if (r == 0)
                r = dir->create(leaf, leaf_len, vfs::S_IFREG | perms, nullptr, &m_file);
            m_stats->files++;
            m_stats->bytes += m_file_size;
            break;
        }
        case vfs::S_IFLNK:
            memset(m_link, 0, sizeof(m_link));
            break;
        default:
            break; // device nodes, FIFOs: created by the kernel itself
        }
        if (r < 0 && r != -err::EXIST)
            klog::warn("initramfs: could not create %s (error %ld)", m_path, -r);
    }

    void end_entry() {
        if ((m_mode & vfs::S_IFMT) == vfs::S_IFLNK) {
            vfs::Inode* parent;
            const char* leaf;
            usize leaf_len;
            long r = vfs::resolve_parent(m_path, nullptr, &parent, &leaf, &leaf_len);
            if (r == 0)
                r = parent->create(leaf, leaf_len, vfs::S_IFLNK | 0777, m_link, nullptr);
            m_stats->symlinks++;
            if (r < 0 && r != -err::EXIST)
                klog::warn("initramfs: could not create %s (error %ld)", m_path, -r);
        }
        m_file = nullptr;
    }
};

} // namespace

long unpack(const void* archive, usize size, Stats* stats) {
    auto* u = new Unpacker(stats);
    long r;
    if (inflate::is_gzip(archive, size)) {
        // A compressed initramfs: decompress and unpack in one stream.
        r = inflate::gunzip(archive, size, [](void* ctx, const u8* data, usize len) {
            return static_cast<Unpacker*>(ctx)->feed(data, len);
        }, u);
    } else {
        r = u->feed(static_cast<const u8*>(archive), size) ? 0 : -err::INVAL;
    }
    if (u->failed())
        r = -err::INVAL;
    delete u;
    return r;
}

} // namespace initramfs
