#include <fs/pipe.hpp>
#include <fs/poll.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>

namespace pipe {

namespace {

constexpr usize CAPACITY = 64 * KiB;
constexpr usize PIPE_BUF = 4096;

class Pipe : public vfs::Inode {
public:
    vfs::Inode* backing = nullptr; // named FIFO: the file system node it belongs to

    Pipe() {
        ino = vfs::next_ino();
        mode = vfs::S_IFIFO | 0600;
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
        bool gone;
        {
            LockGuard guard(m_wq.lock());
            u32 acc = f->flags & vfs::O_ACCMODE;
            if (acc == vfs::O_RDONLY || acc == vfs::O_RDWR)
                m_readers--;
            if (acc == vfs::O_WRONLY || acc == vfs::O_RDWR)
                m_writers--;
            gone = m_readers == 0 && m_writers == 0;
        }
        m_wq.wake_all(); // EOF for readers / EPIPE for writers
        poll::notify();
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

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        if (count == 0)
            return 0;
        for (;;) {
            m_wq.lock().lock();
            if (m_count > 0) {
                usize n = min(count, m_count);
                auto* out = static_cast<u8*>(buf);
                for (usize i = 0; i < n; i++) // the user buffer was validated and faulted in
                    out[i] = m_buf[(m_head + i) % CAPACITY];
                m_head = (m_head + n) % CAPACITY;
                m_count -= n;
                m_wq.lock().unlock();
                m_wq.wake_all(); // room for writers
                poll::notify();
                return static_cast<isize>(n);
            }
            bool eof = m_writers == 0;
            m_wq.lock().unlock();
            if (eof)
                return 0;
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            m_wq.wait_until([&] { return m_count > 0 || m_writers == 0 || signal::interrupted(); });
        }
    }

    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        auto* in = static_cast<const u8*>(buf);
        usize written = 0;
        while (written < count) {
            usize want = count - written;
            bool done_chunk = false;
            {
                LockGuard guard(m_wq.lock());
                if (m_readers == 0) {
                    signal::send(proc::current(), signal::SIGPIPE);
                    return written ? static_cast<isize>(written) : -err::PIPE;
                }
                usize space = CAPACITY - m_count;
                // Writes of up to PIPE_BUF bytes are atomic: wait for room for all of it.
                usize needed = count <= PIPE_BUF ? want : 1;
                if (space >= needed) {
                    usize n = min(want, space);
                    for (usize i = 0; i < n; i++)
                        m_buf[(m_head + m_count + i) % CAPACITY] = in[written + i];
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
                return written ? static_cast<isize>(written) : -err::AGAIN;
            if (signal::interrupted())
                return written ? static_cast<isize>(written) : -err::INTR;
            m_wq.wait_until([&] {
                return CAPACITY - m_count >= (count <= PIPE_BUF ? count - written : 1) || m_readers == 0 ||
                       signal::interrupted();
            });
        }
        return static_cast<isize>(written);
    }

private:
    u8* m_buf;
    usize m_head = 0, m_count = 0;
    u32 m_readers = 0, m_writers = 0;
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
