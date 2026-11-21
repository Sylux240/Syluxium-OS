// /dev/uhid: HID devices implemented by programs (Linux's uhid interface).
// BlueZ uses it for Bluetooth keyboards, mice and game controllers, both
// LE (HOGP) and classic (its input plugin's "UserspaceHID", the default).
// A program writes UHID_CREATE2 with a report descriptor, then UHID_INPUT2
// reports; each open file is one device. Reports go through the same HID
// code as USB and I2C devices, so they appear as evdev devices (and feed
// the console and X). Protocol as in Linux's include/uapi/linux/uhid.h;
// our own code.

#include <core/log.hpp>
#include <drivers/input/uhid.hpp>
#include <drivers/usb/hid_device.hpp>
#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>

namespace uhid {

namespace {

enum : u32 {
    UHID_DESTROY = 1,
    UHID_START = 2,
    UHID_STOP = 3,
    UHID_OPEN = 4,
    UHID_CLOSE = 5,
    UHID_OUTPUT = 6,
    UHID_GET_REPORT = 9,
    UHID_GET_REPORT_REPLY = 10,
    UHID_CREATE2 = 11,
    UHID_INPUT2 = 12,
    UHID_SET_REPORT = 13,
    UHID_SET_REPORT_REPLY = 14,
};
constexpr usize DATA_MAX = 4096, DESCRIPTOR_MAX = 4096;
constexpr u8 FEATURE_REPORT = 2; // rtype: HID_FEATURE_REPORT

struct PACKED Create2 {
    char name[128];
    char phys[64];
    char uniq[64];
    u16 rd_size, bus;
    u32 vendor, product, version, country;
    u8 rd_data[DESCRIPTOR_MAX];
};
struct PACKED Input2 {
    u16 size;
    u8 data[DATA_MAX];
};
struct PACKED GetReport {
    u32 id;
    u8 rnum, rtype;
};
struct PACKED GetReportReply {
    u32 id;
    u16 err, size;
    u8 data[DATA_MAX];
};
struct PACKED SetReport {
    u32 id;
    u8 rnum, rtype;
    u16 size;
    u8 data[DATA_MAX];
};
struct PACKED SetReportReply {
    u32 id;
    u16 err;
};
struct PACKED Event {
    u32 type;
    union {
        Create2 create2;
        Input2 input2;
        u64 start_flags;
        GetReport get_report;
        GetReportReply get_report_reply;
        SetReport set_report;
        SetReportReply set_report_reply;
        u8 raw[sizeof(Create2)];
    } u;
};
static_assert(sizeof(Event) == 4380, "struct uhid_event (the union is padded to 8 bytes, as in Linux)");

constexpr int QUEUE = 16;

// One open /dev/uhid: at most one device.
class Client final : public usb::hid::FeatureIo {
public:
    usb::hid::HidDevice* dev = nullptr;
    sched::WaitQueue wq; // readers, and get/set-report waiters; its lock guards the queue
    Event* queue[QUEUE] = {};
    int head = 0, count = 0;
    u32 next_id = 1;
    // The request waiting for the program's answer (one at a time).
    sched::Mutex request_lock;
    u32 pending_id = 0;
    bool answered = false;
    GetReportReply* answer = nullptr;
    i32 answer_err = 0;

    ~Client() {
        destroy();
        for (int i = 0; i < count; i++)
            delete queue[(head + i) % QUEUE];
        delete answer;
    }

    void push(Event* e) {
        {
            LockGuard guard(wq.lock());
            if (count == QUEUE) { // reader asleep: drop the oldest
                delete queue[head];
                head = (head + 1) % QUEUE;
                count--;
            }
            queue[(head + count++) % QUEUE] = e;
        }
        wq.wake_all();
        poll::notify();
    }

    void destroy() {
        if (!dev)
            return;
        dev->unregister_devices();
        delete dev;
        dev = nullptr;
    }

    // A feature report through the program: UHID_GET_REPORT / UHID_SET_REPORT,
    // answered by its *_REPLY within 5 s.
    long request(u32 type, u8 id, const u8* data, u32 len, u8* out, u32 out_len) {
        LockGuard serial(request_lock);
        auto* e = new Event{};
        e->type = type;
        u32 rid;
        {
            LockGuard guard(wq.lock());
            rid = next_id++;
            pending_id = rid;
            answered = false;
        }
        if (type == UHID_GET_REPORT) {
            e->u.get_report = GetReport{rid, id, FEATURE_REPORT};
        } else {
            e->u.set_report.id = rid;
            e->u.set_report.rnum = id;
            e->u.set_report.rtype = FEATURE_REPORT;
            e->u.set_report.size = static_cast<u16>(len > DATA_MAX ? DATA_MAX : len);
            memcpy(e->u.set_report.data, data, e->u.set_report.size);
        }
        push(e);
        bool ok = wq.wait_until_timeout([&] { return answered; }, 5'000'000'000ull);
        LockGuard guard(wq.lock());
        pending_id = 0;
        if (!ok)
            return -err::TIMEDOUT;
        if (answer_err)
            return -err::IO;
        if (type == UHID_SET_REPORT)
            return static_cast<long>(len);
        u32 n = answer && answer->size < out_len ? answer->size : out_len;
        if (answer)
            memcpy(out, answer->data, n);
        return static_cast<long>(n);
    }

    long get_feature(u8 id, u8* buf, u32 len) override { return request(UHID_GET_REPORT, id, nullptr, 0, buf, len); }
    long set_feature(u8 id, const u8* buf, u32 len) override {
        return request(UHID_SET_REPORT, id, buf, len, nullptr, 0);
    }

    void answer_with(u32 id, i32 errcode, const GetReportReply* reply) {
        LockGuard guard(wq.lock());
        if (!pending_id || id != pending_id)
            return;
        answer_err = errcode;
        if (reply) {
            if (!answer)
                answer = new GetReportReply{};
            memcpy(answer, reply, sizeof(*answer));
        }
        answered = true;
        wq.wake_all();
    }

    long create(const Create2& c) {
        if (dev)
            return -err::EXIST;
        if (c.rd_size == 0 || c.rd_size > DESCRIPTOR_MAX)
            return -err::INVAL;
        auto* d = new usb::hid::HidDevice;
        char name[129];
        memcpy(name, c.name, 128);
        name[128] = 0;
        snprintf(d->name, sizeof(d->name), "%s", name[0] ? name : "uhid");
        if (!d->parse(c.rd_data, c.rd_size)) {
            klog::warn("uhid: %s: report descriptor not usable (%u bytes)", d->name, c.rd_size);
            delete d;
            return -err::INVAL;
        }
        int n = d->register_devices(name[0] ? name : "uhid device", "uhid", c.bus, static_cast<u16>(c.vendor),
                                    static_cast<u16>(c.product), this);
        dev = d;
        klog::info("uhid: %s (%04x:%04x, bus %u): %d input device(s)", d->name, c.vendor & 0xFFFF,
                   c.product & 0xFFFF, c.bus, n);
        // The device is running and opened (BlueZ sends reports after UHID_START).
        auto* start = new Event{};
        start->type = UHID_START;
        push(start);
        auto* open = new Event{};
        open->type = UHID_OPEN;
        push(open);
        return 0;
    }
};

class UhidNode final : public vfs::Inode {
public:
    UhidNode() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0600;
        rdev = (10u << 8) | 239; // misc 239, as on Linux
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override { f->private_data = new Client; }
    void on_release(vfs::File* f) override { delete static_cast<Client*>(f->private_data); }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        auto* c = static_cast<Client*>(f->private_data);
        for (;;) {
            Event* e = nullptr;
            {
                LockGuard guard(c->wq.lock());
                if (c->count) {
                    e = c->queue[c->head];
                    c->head = (c->head + 1) % QUEUE;
                    c->count--;
                }
            }
            if (e) {
                usize n = count < sizeof(Event) ? count : sizeof(Event);
                long r = uaccess::copy_to(buf, e, n);
                delete e;
                return r < 0 ? r : static_cast<isize>(n);
            }
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            c->wq.wait_until([&] { return c->count > 0 || signal::interrupted(); });
        }
    }

    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        auto* c = static_cast<Client*>(f->private_data);
        if (count < 4)
            return -err::INVAL;
        auto* e = new Event{};
        usize n = count < sizeof(Event) ? count : sizeof(Event);
        long r = uaccess::copy_from(e, buf, n);
        long result = static_cast<long>(count);
        if (r < 0) {
            result = r;
        } else {
            switch (e->type) {
            case UHID_CREATE2:
                if (n < offsetof(Event, u) + offsetof(Create2, rd_data) + e->u.create2.rd_size)
                    result = -err::INVAL;
                else if (long cr = c->create(e->u.create2); cr < 0)
                    result = cr;
                break;
            case UHID_DESTROY: c->destroy(); break;
            case UHID_INPUT2:
                if (!c->dev)
                    result = -err::INVAL;
                else if (e->u.input2.size > DATA_MAX)
                    result = -err::INVAL;
                else
                    c->dev->report(e->u.input2.data, e->u.input2.size);
                break;
            case UHID_GET_REPORT_REPLY:
                c->answer_with(e->u.get_report_reply.id, e->u.get_report_reply.err, &e->u.get_report_reply);
                break;
            case UHID_SET_REPORT_REPLY:
                c->answer_with(e->u.set_report_reply.id, e->u.set_report_reply.err, nullptr);
                break;
            default: result = -err::OPNOTSUPP; break; // the legacy CREATE/INPUT (user pointers)
            }
        }
        delete e;
        return static_cast<isize>(result);
    }

    u16 poll(vfs::File* f) override {
        auto* c = static_cast<Client*>(f->private_data);
        LockGuard guard(c->wq.lock());
        return ::poll::POLLOUT | ::poll::POLLWRNORM | (c->count ? ::poll::POLLIN | ::poll::POLLRDNORM : 0);
    }
};

} // namespace

void init() { vfs::add_node("/dev/uhid", new UhidNode); }

} // namespace uhid
