#include <fs/pipe.hpp>
#include <fs/poll.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>

namespace pipe {

namespace {

constexpr usize CAPACITY = 64 * KiB;
constexpr usize PIPE_BUF = 4096;

// Callers pass user buffers (read/write) or kernel ones (sendfile, splice).
bool is_kernel(const void* p) { return reinterpret_cast<uptr>(p) >= mm::USER_END; }

long copy_out(void* dst, const void* src, usize n) {
    if (is_kernel(dst)) {
        memcpy(dst, src, n);
        return 0;
    }
    return uaccess::copy_to(dst, src, n);
}

long copy_in(void* dst, const void* src, usize n) {
    if (is_kernel(src)) {
        memcpy(dst, src, n);
        return 0;
    }
    return uaccess::copy_from(dst, src, n);
}

class Pipe : public vfs::Inode {
public:
    vfs::Inode* backing = nullptr; // named FIFO: the file system node it belongs to

    Pipe() {
        ino = vfs::next_ino();
        mode = vfs::S_IFIFO | 0600;
        // Owned by its creator, as on Linux (fstat shows the user, and
        // permission checks on the pipe itself see the right owner).
        uid = proc::cred().fsuid;
        gid = proc::cred().fsgid;
        atime = mtime = ctime = vfs::now();
        m_buf = static_cast<u8*>(kmalloc(CAPACITY));
    }
    ~Pipe() override { kfree(m_buf); }

    bool ok() const { return m_buf != nullptr; }
    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override {
        u32 acc = f->flags & vfs::O_ACCMODE;
        {
            LockGuard guard(m_wq.lock());
            if (acc == vfs::O_RDONLY || acc == vfs::O_RDWR)
                m_readers++;
            if (acc == vfs::O_WRONLY || acc == vfs::O_RDWR)
                m_writers++;
        }
        m_wq.wake_all();
        poll::notify();
        // A named FIFO opened for one direction waits for the other side
        // (unless non-blocking), as POSIX says.
        if (backing && acc != vfs::O_RDWR && !(f->flags & vfs::O_NONBLOCK)) {
            bool reader = acc == vfs::O_RDONLY;
            m_wq.wait_until([&] { return (reader ? m_writers : m_readers) > 0 || signal::interrupted(); });
        }
    }

    void on_release(vfs::File* f) override {
        // Both ends can be closed at the same time on two CPUs: the one that
        // took the last count away must not free the pipe while the other is
        // still waking sleepers on it. m_releasing counts the closers inside
        // here; the last one out frees it.
        __atomic_add_fetch(&m_releasing, 1, __ATOMIC_ACQ_REL);
        {
            LockGuard guard(m_wq.lock());
            u32 acc = f->flags & vfs::O_ACCMODE;
            if (acc == vfs::O_RDONLY || acc == vfs::O_RDWR)
                m_readers--;
            if (acc == vfs::O_WRONLY || acc == vfs::O_RDWR)
                m_writers--;
        }
        m_wq.wake_all(); // EOF for readers / EPIPE for writers
        poll::notify();
        bool gone = false;
        {
            LockGuard guard(m_wq.lock());
            if (__atomic_sub_fetch(&m_releasing, 1, __ATOMIC_ACQ_REL) == 0)
                gone = m_readers == 0 && m_writers == 0;
        }
        if (gone && !backing) // named FIFOs keep their pipe (and buffered data) for the next open
            delete this;
    }

    u16 poll(vfs::File* f) override {
        LockGuard guard(m_wq.lock());
        if ((f->flags & vfs::O_ACCMODE) == vfs::O_RDWR)
            return (m_count > 0 ? poll::POLLIN : 0) | (m_count < CAPACITY ? poll::POLLOUT : 0);
        if ((f->flags & vfs::O_ACCMODE) == vfs::O_RDONLY) {
            u16 r = m_count > 0 ? poll::POLLIN : 0;
            if (m_writers == 0)
                r |= poll::POLLHUP;
            return r;
        }
        if (m_readers == 0)
            return poll::POLLERR;
        return m_count < CAPACITY ? poll::POLLOUT : 0;
    }

    // User memory is only touched with the pipe's spinlock released: even a
    // buffer that was faulted in can be unmapped or protected by another
    // thread meanwhile (Wine does that), and the page fault then sleeps.
    // The data goes through a kernel bounce buffer instead.
    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        if (count == 0)
            return 0;
        usize chunk = min(count, CAPACITY);
        auto* bounce = static_cast<u8*>(kmalloc(chunk));
        if (!bounce)
            return -err::NOMEM;
        for (;;) {
            m_wq.lock().lock();
            if (m_count > 0) {
                usize n = min(chunk, m_count);
                for (usize i = 0; i < n; i++)
                    bounce[i] = m_buf[(m_head + i) % CAPACITY];
                m_head = (m_head + n) % CAPACITY;
                m_count -= n;
                m_wq.lock().unlock();
                m_wq.wake_all(); // room for writers
                poll::notify();
                long r = copy_out(buf, bounce, n);
                kfree(bounce);
                return r < 0 ? r : static_cast<isize>(n);
            }
            bool eof = m_writers == 0;
            m_wq.lock().unlock();
            isize r = 0;
            if (eof)
                r = 0;
            else if (f->flags & vfs::O_NONBLOCK)
                r = -err::AGAIN;
            else if (signal::interrupted())
                r = -err::INTR;
            else {
                m_wq.wait_until([&] { return m_count > 0 || m_writers == 0 || signal::interrupted(); });
                continue;
            }
            kfree(bounce);
            return r;
        }
    }

    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        auto* in = static_cast<const u8*>(buf);
        usize chunk = min(count, CAPACITY);
        auto* bounce = static_cast<u8*>(kmalloc(chunk ? chunk : 1));
        if (!bounce)
            return -err::NOMEM;
        usize written = 0;
        usize staged = 0, staged_at = 0; // bounce holds in[staged_at .. staged_at + staged)
        auto finish = [&](isize r) {
            kfree(bounce);
            return r;
        };
        while (written < count) {
            if (written >= staged_at + staged) { // refill from user memory (no lock held)
                staged_at = written;
                staged = min(count - written, chunk);
                if (long r = copy_in(bounce, in + written, staged); r < 0)
                    return finish(written ? static_cast<isize>(written) : r);
            }
            usize want = count - written;
            bool done_chunk = false;
            {
                LockGuard guard(m_wq.lock());
                if (m_readers == 0) {
                    signal::send(proc::current(), signal::SIGPIPE);
                    return finish(written ? static_cast<isize>(written) : -err::PIPE);
                }
                usize space = CAPACITY - m_count;
                // Writes of up to PIPE_BUF bytes are atomic: wait for room for all of it.
                usize needed = count <= PIPE_BUF ? want : 1;
                if (space >= needed) {
                    usize n = min(min(want, space), staged_at + staged - written);
                    for (usize i = 0; i < n; i++)
                        m_buf[(m_head + m_count + i) % CAPACITY] = bounce[written - staged_at + i];
                    m_count += n;
                    written += n;
                    done_chunk = true;
                }
            }
            if (done_chunk) {
                m_wq.wake_all(); // data for readers
                poll::notify();
                continue;
            }
            if (f->flags & vfs::O_NONBLOCK)
                return finish(written ? static_cast<isize>(written) : -err::AGAIN);
            if (signal::interrupted())
                return finish(written ? static_cast<isize>(written) : -err::INTR);
            m_wq.wait_until([&] {
                return CAPACITY - m_count >= (count <= PIPE_BUF ? count - written : 1) || m_readers == 0 ||
                       signal::interrupted();
            });
        }
        return finish(static_cast<isize>(written));
    }

private:
    u8* m_buf;
    usize m_head = 0, m_count = 0;
    u32 m_readers = 0, m_writers = 0;
    u32 m_releasing = 0; // closers inside on_release (see there)
    sched::WaitQueue m_wq; // its lock protects all of the above
};

struct Named {
    vfs::Inode* backing;
    Pipe* pipe;
};
Named* g_named = nullptr;
usize g_named_count = 0, g_named_cap = 0;
sched::Mutex g_named_lock;

} // namespace

vfs::Inode* fifo_for(vfs::Inode* node) {
    LockGuard guard(g_named_lock);
    for (usize i = 0; i < g_named_count; i++)
        if (g_named[i].backing == node)
            return g_named[i].pipe;
    if (g_named_count == g_named_cap) {
        usize ncap = g_named_cap ? g_named_cap * 2 : 16;
        auto* n = static_cast<Named*>(kmalloc(ncap * sizeof(Named)));
        if (!n)
            return nullptr;
        if (g_named)
            memcpy(n, g_named, g_named_count * sizeof(Named));
        kfree(g_named);
        g_named = n;
        g_named_cap = ncap;
    }
    auto* p = new Pipe;
    if (!p->ok()) {
        delete p;
        return nullptr;
    }
    p->backing = node;
    p->mode = node->mode;
    p->uid = node->uid;
    p->gid = node->gid;
    g_named[g_named_count++] = Named{node, p};
    return p;
}

long create(u32 flags, vfs::File** read_end, vfs::File** write_end) {
    auto* p = new Pipe;
    if (!p->ok()) {
        delete p;
        return -err::NOMEM;
    }
    u32 status = flags & vfs::O_NONBLOCK;
    *read_end = vfs::open_inode(p, vfs::O_RDONLY | status);
    *write_end = vfs::open_inode(p, vfs::O_WRONLY | status);
    return 0;
}

} // namespace pipe
