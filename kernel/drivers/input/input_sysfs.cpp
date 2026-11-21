// What libudev (and through it libinput, wlroots, TDE's hardware library)
// reads about input devices on Linux, for each /dev/input/eventN:
//
//   /sys/devices/virtual/input/inputN/        name, phys, uniq, properties,
//       uevent (PRODUCT, NAME, EV=, KEY=, ...), id/{bustype,vendor,product,
//       version}, capabilities/{ev,key,rel,abs,msc,sw,led,snd,ff}
//   /sys/devices/virtual/input/inputN/eventN/ uevent (MAJOR, MINOR,
//       DEVNAME=input/eventN), dev, device -> ..
//   /sys/class/input/{inputN,eventN} and /sys/dev/char/13:<64+N> -> those
//   NETLINK_KOBJECT_UEVENT add/remove events (kernel and udev format)
//   /run/udev/data/c13:<64+N>                 the properties udevd's
//       input_id builtin would have stored: ID_INPUT=1, ID_INPUT_KEYBOARD,
//       _KEY, _MOUSE, _TOUCHPAD, _TOUCHSCREEN, _TABLET, _JOYSTICK, _SWITCH
//
// There is no udevd here, so the kernel keeps that database itself. The
// combined devices (event0/event1) get no ID_INPUT: libinput would
// otherwise see every key and motion twice.

#include <boot/limine_requests.hpp>
#include <drivers/input/input_sysfs.hpp>
#include <fs/genfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <net/uevent.hpp>
#include <time/clock.hpp>

namespace input::sysfs {

namespace {

using genfs::GeneratedFile;
using genfs::GeneratedLink;
using genfs::TextBuffer;

constexpr u32 EVDEV_MINOR_BASE = 64;
constexpr u16 BTN_TOOL_FINGER_ = 0x145, BTN_TOOL_PEN_ = 0x140, BTN_STYLUS_ = 0x14B, BTN_TOUCH_ = 0x14A,
              BTN_JOYSTICK_ = 0x120, BTN_GAMEPAD_ = 0x130, BTN_TRIGGER_HAPPY_ = 0x2C0, BTN_MOUSE_ = 0x110,
              ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36;

bool g_ready = false;
bool g_created[64]; // /sys/devices/virtual/input/inputN built (kept across unplugs)

void* ctx_of(u32 slot) { return reinterpret_cast<void*>(static_cast<uptr>(slot)); }
u32 slot_of(const void* c) { return static_cast<u32>(reinterpret_cast<uptr>(c)); }

// A heap copy of the device (the struct is ~2 KiB).
struct Snap {
    Device* d = new Device;
    bool combined = false;
    bool ok = false;
    explicit Snap(const void* c) { ok = d && snapshot(slot_of(c), d, &combined); }
    ~Snap() { delete d; }
};

// Linux's bitmap format: 64-bit words in hex (unpadded), most significant first,
// leading zero words dropped ("0" when empty).
void print_bitmap(TextBuffer& o, const u64* words, int n) {
    int top = n - 1;
    while (top > 0 && !words[top])
        top--;
    for (int i = top; i >= 0; i--)
        o.printf(i == top ? "%lx" : " %lx", words[i]);
}

enum class Cap { Ev, Key, Rel, Abs, Msc, Sw, Led, Snd, Ff };

void print_cap(TextBuffer& o, const Device& d, Cap c) {
    u64 zero = 0;
    switch (c) {
    case Cap::Ev: print_bitmap(o, &d.ev_bits, 1); break;
    case Cap::Key: print_bitmap(o, d.key_bits, sizeof(d.key_bits) / 8); break;
    case Cap::Rel: print_bitmap(o, &d.rel_bits, 1); break;
    case Cap::Abs: print_bitmap(o, &d.abs_bits, 1); break;
    case Cap::Msc: print_bitmap(o, &d.msc_bits, 1); break;
    case Cap::Sw: print_bitmap(o, &d.sw_bits, 1); break;
    default: print_bitmap(o, &zero, 1); break;
    }
}

template <Cap C>
bool gen_cap(TextBuffer& o, const void* c) {
    Snap s(c);
    if (!s.ok)
        return false;
    print_cap(o, *s.d, C);
    o.append("\n");
    return true;
}

bool gen_name(TextBuffer& o, const void* c) {
    Snap s(c);
    if (!s.ok)
        return false;
    o.printf("%s\n", s.d->name);
    return true;
}
bool gen_empty(TextBuffer& o, const void* c) {
    Snap s(c);
    if (!s.ok)
        return false;
    o.append("\n");
    return true;
}
bool gen_props(TextBuffer& o, const void* c) {
    Snap s(c);
    if (!s.ok)
        return false;
    o.printf("%x\n", s.d->props);
    return true;
}
template <int Field>
bool gen_id(TextBuffer& o, const void* c) {
    Snap s(c);
    if (!s.ok)
        return false;
    const u16 v[4] = {s.d->bustype, s.d->vendor, s.d->product, s.d->version};
    o.printf("%04x\n", v[Field]);
    return true;
}

bool gen_input_uevent(TextBuffer& o, const void* c) {
    Snap s(c);
    if (!s.ok)
        return false;
    const Device& d = *s.d;
    o.printf("PRODUCT=%x/%x/%x/%x\nNAME=\"%s\"\nPHYS=\"\"\nUNIQ=\"\"\nPROP=%x\nEV=", d.bustype, d.vendor, d.product,
             d.version, d.name, d.props);
    print_cap(o, d, Cap::Ev);
    struct {
        u16 ev;
        const char* name;
        Cap cap;
    } const parts[] = {{EV_KEY, "KEY", Cap::Key}, {EV_REL, "REL", Cap::Rel}, {EV_ABS, "ABS", Cap::Abs},
                       {EV_MSC, "MSC", Cap::Msc}, {EV_SW, "SW", Cap::Sw}};
    for (auto& p : parts)
        if (d.ev_bits & (1ull << p.ev)) {
            o.printf("\n%s=", p.name);
            print_cap(o, d, p.cap);
        }
    o.append("\n");
    return true;
}

bool gen_event_uevent(TextBuffer& o, const void* c) {
    u32 n = slot_of(c);
    o.printf("MAJOR=13\nMINOR=%u\nDEVNAME=input/event%u\n", EVDEV_MINOR_BASE + n, n);
    return true;
}
bool gen_event_dev(TextBuffer& o, const void* c) {
    o.printf("13:%u\n", EVDEV_MINOR_BASE + slot_of(c));
    return true;
}

// The udev database entry: what udevd's input_id builtin decides from the
// capabilities (systemd src/udev/udev-builtin-input_id.c, same tests).
bool gen_udev_db(TextBuffer& o, const void* c) {
    Snap s(c);
    if (!s.ok)
        return false;
    const Device& d = *s.d;
    o.printf("I:%lu\n", clock::now_ns() / 1000);
    if (s.combined) {
        o.append("E:LIBINPUT_IGNORE_DEVICE=1\n"); // the real devices are listed too
        return true;
    }
    auto ev = [&](u16 t) { return (d.ev_bits >> t) & 1; };
    auto abs = [&](u16 a) { return (d.abs_bits >> a) & 1; };
    auto rel = [&](u16 r) { return (d.rel_bits >> r) & 1; };
    bool has_xy = ev(EV_ABS) && abs(ABS_X) && abs(ABS_Y);
    bool has_mt = ev(EV_ABS) && abs(ABS_MT_POSITION_X) && abs(ABS_MT_POSITION_Y);
    bool direct = d.props & (1u << INPUT_PROP_DIRECT);
    bool finger = d.has_key(BTN_TOOL_FINGER_), pen = d.has_key(BTN_TOOL_PEN_) || d.has_key(BTN_STYLUS_);
    bool touch = d.has_key(BTN_TOUCH_);
    bool joystick_buttons = false;
    for (u16 b = BTN_JOYSTICK_; b < BTN_JOYSTICK_ + 0x20 && !joystick_buttons; b++)
        joystick_buttons = d.has_key(b);
    for (u16 b = BTN_TRIGGER_HAPPY_; b < BTN_TRIGGER_HAPPY_ + 0x28 && !joystick_buttons; b++)
        joystick_buttons = d.has_key(b);
    bool mouse_button = d.has_key(BTN_MOUSE_);

    bool tablet = false, touchpad = false, touchscreen = false, joystick = false, mouse = false;
    if (has_xy || has_mt) {
        if (pen)
            tablet = true;
        else if (finger && !direct)
            touchpad = true;
        else if (mouse_button)
            mouse = true; // absolute mice (VM tablets)
        else if (touch || direct)
            touchscreen = true;
        else if (joystick_buttons || d.has_key(BTN_GAMEPAD_))
            joystick = true;
    }
    if (ev(EV_REL) && rel(REL_X) && rel(REL_Y) && mouse_button)
        mouse = true;
    // Keys: anything below BTN_MISC is a key; the first 32 codes (Esc,
    // digits, Q..) all present make a full keyboard.
    bool key = false, keyboard = true;
    for (u16 k = 1; k < BTN_MISC && !key; k++)
        key = d.has_key(k);
    for (u16 k = 1; k < 32; k++)
        keyboard &= d.has_key(k);
    keyboard &= key;
    bool sw = ev(EV_SW) && d.sw_bits;

    o.append("E:ID_INPUT=1\n");
    if (keyboard)
        o.append("E:ID_INPUT_KEYBOARD=1\n");
    if (key)
        o.append("E:ID_INPUT_KEY=1\n");
    if (mouse)
        o.append("E:ID_INPUT_MOUSE=1\n");
    if (touchpad)
        o.append("E:ID_INPUT_TOUCHPAD=1\n");
    if (touchscreen)
        o.append("E:ID_INPUT_TOUCHSCREEN=1\n");
    if (tablet)
        o.append("E:ID_INPUT_TABLET=1\n");
    if (joystick)
        o.append("E:ID_INPUT_JOYSTICK=1\n");
    if (sw)
        o.append("E:ID_INPUT_SWITCH=1\n");
    o.append("E:ID_SEAT=seat0\nG:seat\nG:uaccess\nQ:seat\nQ:uaccess\nV:1\n");
    return true;
}

template <usize N>
bool link_to(char* out, usize size, const void* c, const char (&fmt)[N]) {
    u32 n = slot_of(c);
    snprintf(out, size, fmt, n, n);
    return true;
}
bool link_class_input(char* out, usize size, const void* c) {
    return link_to(out, size, c, "../../devices/virtual/input/input%u");
}
bool link_class_event(char* out, usize size, const void* c) {
    return link_to(out, size, c, "../../devices/virtual/input/input%u/event%u");
}
bool link_dev_char(char* out, usize size, const void* c) {
    return link_to(out, size, c, "../../devices/virtual/input/input%u/event%u");
}
bool link_subsystem_from_input(char* out, usize size, const void*) {
    snprintf(out, size, "../../../../class/input");
    return true;
}
bool link_subsystem_from_event(char* out, usize size, const void*) {
    snprintf(out, size, "../../../../../class/input");
    return true;
}
bool link_parent(char* out, usize size, const void* c) {
    return link_to(out, size, c, "../../input%u");
}

void add(const char* dir, const char* name, vfs::Inode* node) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    vfs::add_node(path, node);
}

void create_device_dirs(u32 n) {
    void* c = ctx_of(n);
    char dir[96];
    snprintf(dir, sizeof(dir), "/sys/devices/virtual/input/input%u", n);
    vfs::mkdir_p(dir, 0755);
    add(dir, "uevent", new GeneratedFile(gen_input_uevent, c));
    add(dir, "name", new GeneratedFile(gen_name, c));
    add(dir, "phys", new GeneratedFile(gen_empty, c));
    add(dir, "uniq", new GeneratedFile(gen_empty, c));
    add(dir, "properties", new GeneratedFile(gen_props, c));
    add(dir, "subsystem", new GeneratedLink(link_subsystem_from_input, c));
    char sub[128];
    snprintf(sub, sizeof(sub), "%s/id", dir);
    vfs::mkdir_p(sub, 0755);
    add(sub, "bustype", new GeneratedFile(gen_id<0>, c));
    add(sub, "vendor", new GeneratedFile(gen_id<1>, c));
    add(sub, "product", new GeneratedFile(gen_id<2>, c));
    add(sub, "version", new GeneratedFile(gen_id<3>, c));
    snprintf(sub, sizeof(sub), "%s/capabilities", dir);
    vfs::mkdir_p(sub, 0755);
    add(sub, "ev", new GeneratedFile(gen_cap<Cap::Ev>, c));
    add(sub, "key", new GeneratedFile(gen_cap<Cap::Key>, c));
    add(sub, "rel", new GeneratedFile(gen_cap<Cap::Rel>, c));
    add(sub, "abs", new GeneratedFile(gen_cap<Cap::Abs>, c));
    add(sub, "msc", new GeneratedFile(gen_cap<Cap::Msc>, c));
    add(sub, "sw", new GeneratedFile(gen_cap<Cap::Sw>, c));
    add(sub, "led", new GeneratedFile(gen_cap<Cap::Led>, c));
    add(sub, "snd", new GeneratedFile(gen_cap<Cap::Snd>, c));
    add(sub, "ff", new GeneratedFile(gen_cap<Cap::Ff>, c));
    snprintf(sub, sizeof(sub), "%s/event%u", dir, n);
    vfs::mkdir_p(sub, 0755);
    add(sub, "uevent", new GeneratedFile(gen_event_uevent, c));
    add(sub, "dev", new GeneratedFile(gen_event_dev, c));
    add(sub, "subsystem", new GeneratedLink(link_subsystem_from_event, c));
    add(sub, "device", new GeneratedLink(link_parent, c));
}

void path_of(char* out, usize size, const char* prefix, u32 n) { snprintf(out, size, "%s13:%u", prefix, EVDEV_MINOR_BASE + n); }

// Hot-plug events for libudev monitors (libinput, wlroots): one for
// inputN, one for eventN, as Linux sends them.
void announce(u32 n, const char* action) {
    TextBuffer db;
    if (!gen_udev_db(db, ctx_of(n)))
        return;
    // The database's "E:KEY=VALUE" lines are the udev properties.
    constexpr int MAX_PROPS = 16;
    char props[MAX_PROPS][64];
    const char* udev_env[MAX_PROPS + 4] = {};
    int np = 0;
    char line[96];
    usize pos = 0;
    while (pos < db.size() && np < MAX_PROPS) {
        usize l = 0;
        while (pos < db.size() && db.data()[pos] != '\n' && l + 1 < sizeof(line))
            line[l++] = db.data()[pos++];
        line[l] = 0;
        pos++;
        if (line[0] == 'E' && line[1] == ':') {
            snprintf(props[np], sizeof(props[np]), "%s", line + 2);
            udev_env[np] = props[np];
            np++;
        }
    }
    static const char* const tags[] = {"seat", "uaccess", nullptr};
    char devpath[80];
    snprintf(devpath, sizeof(devpath), "/devices/virtual/input/input%u", n);
    net::uevent_broadcast(action, devpath, "input", nullptr, udev_env, nullptr);

    char major[16] = "MAJOR=13", minor[16], kname[32], uname[40];
    snprintf(minor, sizeof(minor), "MINOR=%u", EVDEV_MINOR_BASE + n);
    snprintf(kname, sizeof(kname), "DEVNAME=input/event%u", n);
    snprintf(uname, sizeof(uname), "DEVNAME=/dev/input/event%u", n);
    const char* kernel_env[] = {major, minor, kname, nullptr};
    udev_env[np] = major;
    udev_env[np + 1] = minor;
    udev_env[np + 2] = uname;
    udev_env[np + 3] = nullptr;
    snprintf(devpath, sizeof(devpath), "/devices/virtual/input/input%u/event%u", n, n);
    net::uevent_broadcast(action, devpath, "input", kernel_env, udev_env, tags);
}

} // namespace

void init() {
    // Boot option for comparisons: no /sys/class/input, no udev database.
    if (strstr(boot::cmdline(), "noinputsysfs"))
        return;
    vfs::mkdir_p("/sys/class/input", 0755);
    vfs::mkdir_p("/sys/devices/virtual/input", 0755);
    vfs::mkdir_p("/sys/dev/char", 0755);
    vfs::mkdir_p("/run/udev/data", 0755);
    // libudev only subscribes its monitors to udevd's broadcasts (which the
    // kernel sends, see net/uevent.hpp) when udevd's control socket exists.
    genfs::static_file("/run/udev/control", "", 0600);
    g_ready = true;
}

void added(u32 n) {
    if (!g_ready || n >= 64)
        return;
    if (!g_created[n]) {
        create_device_dirs(n);
        g_created[n] = true;
    }
    void* c = ctx_of(n);
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/input/input%u", n);
    vfs::add_node(path, new GeneratedLink(link_class_input, c));
    snprintf(path, sizeof(path), "/sys/class/input/event%u", n);
    vfs::add_node(path, new GeneratedLink(link_class_event, c));
    path_of(path, sizeof(path), "/sys/dev/char/", n);
    vfs::add_node(path, new GeneratedLink(link_dev_char, c));
    path_of(path, sizeof(path), "/run/udev/data/c", n);
    vfs::add_node(path, new GeneratedFile(gen_udev_db, c, 0644));
    announce(n, "add");
}

void removed(u32 n) {
    if (!g_ready || n >= 64)
        return;
    announce(n, "remove"); // while the device's properties can still be read
    char path[96];
    path_of(path, sizeof(path), "/run/udev/data/c", n);
    vfs::remove_node(path);
    path_of(path, sizeof(path), "/sys/dev/char/", n);
    vfs::remove_node(path);
    snprintf(path, sizeof(path), "/sys/class/input/event%u", n);
    vfs::remove_node(path);
    snprintf(path, sizeof(path), "/sys/class/input/input%u", n);
    vfs::remove_node(path);
}

} // namespace input::sysfs
