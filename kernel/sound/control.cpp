// Control devices (/dev/snd/controlC<n>): card information, PCM device
// discovery (what `aplay -l` lists) and the mixer elements amixer/alsamixer
// and the desktop's volume control use, with change notifications.

#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sound/sound.hpp>

namespace sound {

namespace {

using namespace abi;

constexpr u32 EVENT_QUEUE = 64;

// One open control file: whether it subscribed to events, and its queue.
struct Client {
    bool subscribed = false;
    CtlEvent queue[EVENT_QUEUE];
    u32 head = 0, count = 0;
    Client* next = nullptr;
};

// Per-card subscriber list (Card::events points at one of these).
struct EventHub {
    sched::WaitQueue wait; // its lock protects the client list and queues
    Client* clients = nullptr;
};

EventHub* hub(Card* c) { return static_cast<EventHub*>(c->events); }

void fill_id(const Control* c, ElemId* id) {
    memset(id, 0, sizeof(*id));
    id->numid = c->numid;
    id->iface = c->iface;
    id->device = c->device;
    copy_name(id->name, c->name, sizeof(id->name));
    id->index = c->index;
}

Control* find(Card* card, const ElemId& id) {
    if (id.numid) {
        if (id.numid > card->control_count)
            return nullptr;
        return card->controls[id.numid - 1];
    }
    for (u32 i = 0; i < card->control_count; i++) {
        Control* c = card->controls[i];
        if (c->iface == id.iface && c->device == id.device && c->index == id.index &&
            strncmp(c->name, reinterpret_cast<const char*>(id.name), sizeof(id.name)) == 0)
            return c;
    }
    return nullptr;
}

void queue_event(Card* card, Control* c, u32 mask) {
    EventHub* h = hub(card);
    if (!h)
        return;
    bool any = false;
    {
        LockGuard guard(h->wait.lock());
        for (Client* cl = h->clients; cl; cl = cl->next) {
            if (!cl->subscribed)
                continue;
            // Coalesce with a pending event for the same element, like Linux.
            bool merged = false;
            for (u32 i = 0; i < cl->count; i++) {
                CtlEvent& e = cl->queue[(cl->head + i) % EVENT_QUEUE];
                if (e.id.numid == c->numid) {
                    e.mask |= mask;
                    merged = true;
                    break;
                }
            }
            if (merged || cl->count == EVENT_QUEUE)
                continue;
            CtlEvent& e = cl->queue[(cl->head + cl->count++) % EVENT_QUEUE];
            memset(&e, 0, sizeof(e));
            e.type = 0;
            e.mask = mask;
            fill_id(c, &e.id);
            any = true;
        }
    }
    if (any) {
        h->wait.wake_all();
        poll::notify();
    }
}

template <typename T>
long get_arg(T* out, u64 arg) { return uaccess::copy_from(out, reinterpret_cast<const void*>(arg), sizeof(T)); }
template <typename T>
long put_arg(u64 arg, const T& v) { return uaccess::copy_to(reinterpret_cast<void*>(arg), &v, sizeof(T)); }

class ControlNode : public vfs::Inode {
public:
    explicit ControlNode(Card* card) : m_card(card) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0660;
        gid = vfs::GID_AUDIO;
        rdev = (116u << 8) | static_cast<u32>(card->number * 32);
        gid = 29; // "audio"
        atime = mtime = ctime = vfs::now();
    }

    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override {
        auto* c = new Client;
        LockGuard guard(hub(m_card)->wait.lock());
        c->next = hub(m_card)->clients;
        hub(m_card)->clients = c;
        f->private_data = c;
    }

    void on_release(vfs::File* f) override {
        auto* c = static_cast<Client*>(f->private_data);
        {
            LockGuard guard(hub(m_card)->wait.lock());
            Client** link = &hub(m_card)->clients;
            while (*link && *link != c)
                link = &(*link)->next;
            if (*link)
                *link = c->next;
        }
        delete c;
    }

    // Reading returns queued struct snd_ctl_event records.
    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        auto* c = static_cast<Client*>(f->private_data);
        EventHub* h = hub(m_card);
        if (count < sizeof(CtlEvent))
            return -err::INVAL;
        if (!c->subscribed)
            return -snderr::BADFD;
        for (;;) {
            h->wait.lock().lock();
            if (c->count) {
                usize n = min<usize>(count / sizeof(CtlEvent), c->count);
                CtlEvent tmp[8];
                n = min<usize>(n, array_size(tmp));
                for (usize i = 0; i < n; i++) {
                    tmp[i] = c->queue[c->head];
                    c->head = (c->head + 1) % EVENT_QUEUE;
                }
                c->count -= static_cast<u32>(n);
                h->wait.lock().unlock();
                if (long r = uaccess::copy_to(buf, tmp, n * sizeof(CtlEvent)); r < 0)
                    return r;
                return static_cast<isize>(n * sizeof(CtlEvent));
            }
            h->wait.lock().unlock();
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            h->wait.wait_until([&] { return c->count > 0 || signal::interrupted(); });
        }
    }

    u16 poll(vfs::File* f) override {
        auto* c = static_cast<Client*>(f->private_data);
        LockGuard guard(hub(m_card)->wait.lock());
        return c->count ? (poll::POLLIN | poll::POLLRDNORM) : 0;
    }

    long ioctl_file(vfs::File* f, u64 req, u64 arg) override;

private:
    long elem_info(u64 arg);
    long elem_read(u64 arg);
    long elem_write(u64 arg);
    long tlv_read(u64 arg);
    long pcm_info(u64 arg);

    Card* m_card;
};

long ControlNode::elem_info(u64 arg) {
    auto* info = new ElemInfo;
    long r = get_arg(info, arg);
    Control* c = r == 0 ? find(m_card, info->id) : nullptr;
    if (r == 0 && !c)
        r = -err::NOENT;
    if (r == 0) {
        memset(info, 0, sizeof(*info));
        fill_id(c, &info->id);
        info->type = c->type;
        info->access = ACCESS_READWRITE | (c->has_db ? ACCESS_TLV_READ : 0);
        info->count = c->count;
        info->owner = -1;
        if (c->type == ELEM_TYPE_INTEGER) {
            info->value.integer.min = c->min;
            info->value.integer.max = c->max;
            info->value.integer.step = c->step;
        } else { // boolean
            info->value.integer.min = 0;
            info->value.integer.max = 1;
            info->value.integer.step = 0;
        }
        r = put_arg(arg, *info);
    }
    delete info;
    return r;
}

long ControlNode::elem_read(u64 arg) {
    auto* v = new ElemValue;
    long r = get_arg(&v->id, arg);
    Control* c = r == 0 ? find(m_card, v->id) : nullptr;
    if (r == 0 && !c)
        r = -err::NOENT;
    if (r == 0) {
        memset(v, 0, sizeof(*v));
        fill_id(c, &v->id);
        LockGuard guard(m_card->control_lock);
        r = c->get(c, v->value.integer);
    }
    if (r >= 0)
        r = put_arg(arg, *v);
    delete v;
    return r;
}

long ControlNode::elem_write(u64 arg) {
    auto* v = new ElemValue;
    long r = get_arg(v, arg);
    Control* c = r == 0 ? find(m_card, v->id) : nullptr;
    if (r == 0 && !c)
        r = -err::NOENT;
    if (r == 0 && !c->put)
        r = -err::PERM;
    if (r == 0) {
        i64 lo = c->type == ELEM_TYPE_INTEGER ? c->min : 0;
        i64 hi = c->type == ELEM_TYPE_INTEGER ? c->max : 1;
        for (u32 i = 0; i < c->count; i++)
            if (v->value.integer[i] < lo || v->value.integer[i] > hi)
                r = -err::INVAL;
    }
    if (r == 0) {
        long changed;
        {
            LockGuard guard(m_card->control_lock);
            changed = c->put(c, v->value.integer);
        }
        if (changed < 0)
            r = changed;
        else if (changed > 0)
            queue_event(m_card, c, CTL_EVENT_MASK_VALUE);
        if (r == 0) {
            fill_id(c, &v->id);
            r = put_arg(arg, v->id);
        }
    }
    delete v;
    return r;
}

// TLV read: the dB scale of a volume control, so mixers can show decibels.
long ControlNode::tlv_read(u64 arg) {
    CtlTlv hdr;
    if (long r = get_arg(&hdr, arg); r < 0)
        return r;
    ElemId id{};
    id.numid = hdr.numid;
    Control* c = find(m_card, id);
    if (!c)
        return -err::NOENT;
    if (!c->has_db)
        return -err::NXIO;
    u32 tlv[4] = {TLVT_DB_SCALE, 8, static_cast<u32>(c->db_min),
                  (static_cast<u32>(c->db_step) & 0xFFFF) | (c->db_mute_at_min ? TLV_DB_SCALE_MUTE : 0)};
    if (hdr.length < sizeof(tlv))
        return -err::NOMEM;
    return uaccess::copy_to(reinterpret_cast<void*>(arg + sizeof(CtlTlv)), tlv, sizeof(tlv));
}

long ControlNode::pcm_info(u64 arg) {
    auto* info = new PcmInfo;
    long r = get_arg(info, arg);
    if (r == 0) {
        Pcm* pcm = nullptr;
        for (u32 i = 0; i < m_card->pcm_count; i++)
            if (m_card->pcms[i]->device == info->device)
                pcm = m_card->pcms[i];
        int stream = info->stream;
        if (!pcm || (info->subdevice != 0 && info->subdevice != 0xFFFFFFFFu))
            r = -err::NXIO;
        else if (stream < 0 || stream > 1 || !pcm->streams[stream])
            r = -err::NOENT;
        if (r == 0) {
            Substream* s = pcm->streams[stream];
            memset(info, 0, sizeof(*info));
            info->device = pcm->device;
            info->stream = stream;
            info->card = m_card->number;
            copy_name(info->id, pcm->id, sizeof(info->id));
            copy_name(info->name, pcm->name, sizeof(info->name));
            copy_name(info->subname, "subdevice #0", sizeof(info->subname));
            info->subdevices_count = 1;
            info->subdevices_avail = s->open ? 0 : 1;
            r = put_arg(arg, *info);
        }
    }
    delete info;
    return r;
}

long ControlNode::ioctl_file(vfs::File* f, u64 req, u64 arg) {
    if (ioc_type(req) != 'U')
        return -err::NOTTY;
    auto* client = static_cast<Client*>(f->private_data);
    switch (ioc_nr(req)) {
    case CTL_PVERSION:
        return put_arg<i32>(arg, CTL_VERSION);
    case CTL_CARD_INFO: {
        auto* info = new CtlCardInfo{};
        info->card = m_card->number;
        copy_name(info->id, m_card->id, sizeof(info->id));
        copy_name(info->driver, m_card->driver, sizeof(info->driver));
        copy_name(info->name, m_card->name, sizeof(info->name));
        copy_name(info->longname, m_card->longname, sizeof(info->longname));
        copy_name(info->mixername, m_card->mixername, sizeof(info->mixername));
        copy_name(info->components, m_card->components, sizeof(info->components));
        long r = put_arg(arg, *info);
        delete info;
        return r;
    }
    case CTL_ELEM_LIST: {
        ElemList list;
        if (long r = get_arg(&list, arg); r < 0)
            return r;
        list.count = m_card->control_count;
        list.used = 0;
        for (u32 i = list.offset; i < m_card->control_count && list.used < list.space; i++) {
            ElemId id;
            fill_id(m_card->controls[i], &id);
            if (long r = uaccess::copy_to(reinterpret_cast<void*>(list.pids + list.used * sizeof(ElemId)), &id, sizeof(id)); r < 0)
                return r;
            list.used++;
        }
        return put_arg(arg, list);
    }
    case CTL_ELEM_INFO:
        return elem_info(arg);
    case CTL_ELEM_READ:
        return elem_read(arg);
    case CTL_ELEM_WRITE:
        return elem_write(arg);
    case CTL_ELEM_LOCK:
    case CTL_ELEM_UNLOCK:
        return 0; // single-user system: locking always succeeds
    case CTL_SUBSCRIBE_EVENTS: {
        i32 v;
        if (long r = get_arg(&v, arg); r < 0)
            return r;
        if (v < 0) // query
            return put_arg<i32>(arg, client->subscribed ? 1 : 0);
        LockGuard guard(hub(m_card)->wait.lock());
        client->subscribed = v > 0;
        if (!client->subscribed)
            client->count = 0;
        return 0;
    }
    case CTL_ELEM_ADD:
    case CTL_ELEM_REPLACE:
    case CTL_ELEM_REMOVE:
        return -err::PERM; // user-defined controls (softvol) aren't supported yet
    case CTL_TLV_READ:
        return tlv_read(arg);
    case CTL_TLV_WRITE:
    case CTL_TLV_COMMAND:
        return -err::NXIO;
    case CTL_PCM_NEXT_DEVICE: {
        i32 dev;
        if (long r = get_arg(&dev, arg); r < 0)
            return r;
        i32 next = -1;
        for (u32 i = 0; i < m_card->pcm_count; i++) {
            i32 d = static_cast<i32>(m_card->pcms[i]->device);
            if (d > dev && (next < 0 || d < next))
                next = d;
        }
        return put_arg(arg, next);
    }
    case CTL_PCM_INFO:
        return pcm_info(arg);
    case CTL_PCM_PREFER_SUBDEVICE:
    case CTL_RAWMIDI_PREFER_SUBDEVICE:
        return 0;
    case CTL_HWDEP_NEXT_DEVICE:
    case CTL_RAWMIDI_NEXT_DEVICE:
    case CTL_UMP_NEXT_DEVICE:
        return put_arg<i32>(arg, -1); // none
    case CTL_HWDEP_INFO:
    case CTL_RAWMIDI_INFO:
        return -err::NXIO;
    case CTL_POWER:
        return 0;
    case CTL_POWER_STATE:
        return put_arg<i32>(arg, 0); // D0: fully on
    default:
        return -err::NOTTY;
    }
}

} // namespace

Control* control_add(Card* card, const Control& tmpl) {
    if (card->control_count >= MAX_CONTROLS)
        return nullptr;
    auto* c = new Control(tmpl);
    c->card = card;
    c->numid = card->control_count + 1;
    card->controls[card->control_count++] = c;
    return c;
}

void control_notify(Control* c) { queue_event(c->card, c, CTL_EVENT_MASK_VALUE); }

namespace internal {

void register_control_node(Card* card, const char* path) {
    if (!card->events)
        card->events = new EventHub;
    vfs::add_node(path, new ControlNode(card));
}

} // namespace internal

} // namespace sound
