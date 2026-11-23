#pragma once

#include <fs/vfs.hpp>

// Building blocks for synthetic filesystems (/proc, /sys): files whose
// contents are generated on every read, and symlinks with computed targets.
namespace genfs {

// Growable text buffer with printf.
class TextBuffer {
public:
    ~TextBuffer();
    void append(const char* s, usize len);
    void append(const char* s);
    void printf(const char* fmt, ...) PRINTF_LIKE(2, 3);
    const char* data() const { return m_data; }
    usize size() const { return m_size; }

private:
    char* m_data = nullptr;
    usize m_size = 0, m_capacity = 0;
};

// A read-only file whose text comes from `generate(buf, ctx)`. Returning
// false means "the thing this file describes is gone" (-ESRCH on read).
class GeneratedFile : public vfs::Inode {
public:
    using Generator = bool (*)(TextBuffer& out, const void* ctx);
    GeneratedFile(Generator gen, const void* ctx, u32 perms = 0444);
    isize read(u64 offset, void* buf, usize count) override;
    bool seekable() const override { return true; }
    bool epoll_regular() const override { return true; }
    // Edge-triggered epoll on /proc files (libmount watches mountinfo with
    // EPOLLIN | EPOLLET): one event, then another only when the mount table
    // changes, as on Linux. Without it libmount re-read mountinfo endlessly.
    u64 poll_generation() const override { return 1 + vfs::mount_generation(); }

private:
    Generator m_gen;
    const void* m_ctx;
};

// A generated file that can also be written (module parameters,
// /sys/module/<module>/parameters/<name>): `write(text, len, ctx)` returns
// bytes taken or a negative errno.
class ControlFile : public GeneratedFile {
public:
    using Writer = isize (*)(const char* text, usize len, const void* ctx);
    ControlFile(Generator gen, Writer writer, const void* ctx, u32 perms = 0644)
        : GeneratedFile(gen, ctx, perms), m_writer(writer), m_wctx(ctx) {}
    isize write(u64 offset, const void* buf, usize count) override {
        return m_writer(static_cast<const char*>(buf), count, m_wctx);
    }

private:
    Writer m_writer;
    const void* m_wctx;
};

// A symlink whose target is computed when it's followed.
class GeneratedLink : public vfs::Inode {
public:
    using Target = bool (*)(char* out, usize size, const void* ctx);
    GeneratedLink(Target target, const void* ctx);
    isize readlink(char* buf, usize size) override;

private:
    Target m_target;
    const void* m_ctx;
};

// Create a small static text file (ramfs) at `path` with `text`.
void static_file(const char* path, const char* text, u32 perms = 0444);

} // namespace genfs
