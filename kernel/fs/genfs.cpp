#include <fs/genfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace genfs {

TextBuffer::~TextBuffer() { kfree(m_data); }

void TextBuffer::append(const char* s, usize len) {
    if (m_size + len + 1 > m_capacity) {
        usize cap = max<usize>(m_capacity * 2, max<usize>(m_size + len + 1, 256));
        auto* p = static_cast<char*>(krealloc(m_data, cap));
        if (!p)
            return; // out of memory: the file just comes out short
        m_data = p;
        m_capacity = cap;
    }
    memcpy(m_data + m_size, s, len);
    m_size += len;
    m_data[m_size] = 0;
}

void TextBuffer::append(const char* s) { append(s, strlen(s)); }

namespace {
void buffer_sink(char c, void* ctx) { static_cast<TextBuffer*>(ctx)->append(&c, 1); }
} // namespace

void TextBuffer::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vformat(buffer_sink, this, fmt, args);
    va_end(args);
}

GeneratedFile::GeneratedFile(Generator gen, const void* ctx, u32 perms) : m_gen(gen), m_ctx(ctx) {
    ino = vfs::next_ino();
    mode = vfs::S_IFREG | perms;
    size = 0; // like Linux /proc: size unknown until read
    atime = mtime = ctime = vfs::now();
}

isize GeneratedFile::read(u64 offset, void* buf, usize count) {
    TextBuffer text;
    if (!m_gen(text, m_ctx))
        return -err::SRCH;
    if (offset >= text.size())
        return 0;
    usize n = min<u64>(count, text.size() - offset);
    memcpy(buf, text.data() + offset, n);
    return static_cast<isize>(n);
}

GeneratedLink::GeneratedLink(Target target, const void* ctx) : m_target(target), m_ctx(ctx) {
    ino = vfs::next_ino();
    mode = vfs::S_IFLNK | 0777;
    atime = mtime = ctime = vfs::now();
}

isize GeneratedLink::readlink(char* buf, usize bufsize) {
    char target[512];
    if (!m_target(target, sizeof(target), m_ctx))
        return -err::NOENT;
    usize n = min<usize>(strlen(target), bufsize);
    memcpy(buf, target, n);
    return static_cast<isize>(n);
}

void static_file(const char* path, const char* text, u32 perms) {
    vfs::create_file(path, text, strlen(text), perms);
}

} // namespace genfs
