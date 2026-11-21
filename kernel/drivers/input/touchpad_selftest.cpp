// Boot-time self-test of the touchpad decoders, with packets laid out as
// the hardware sends them (QEMU emulates none of these pads): a HID
// precision touchpad (Microsoft's report layout, as on I2C-HID pads of
// Synaptics/Elan/ALPS), Synaptics PS/2 (incl. the ThinkPad TrackPoint
// pass-through and its buttons wired to the pad) and Elantech v4 PS/2.

#include <core/log.hpp>
#include <core/panic.hpp>
#include <drivers/input/ps2_touchpad.hpp>
#include <drivers/usb/hid_touchpad.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace input {

namespace {

// Precision touchpad: report 1 = two fingers (confidence, tip, 3-bit contact
// ID, 16-bit X 0..1199 over 10 cm, Y 0..723 over 7 cm), contact count,
// button 1; feature 2 = contact count max / pad type; device configuration:
// feature 3 = input mode, feature 4 = surface/button switches.
#define PTP_FINGER                                                                                                     \
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02, 0x15, 0x00, 0x25, 0x01, 0x09, 0x47, 0x09, 0x42, 0x95, 0x02, 0x75, 0x01, 0x81, \
        0x02, 0x95, 0x01, 0x75, 0x03, 0x25, 0x03, 0x09, 0x51, 0x81, 0x02, 0x75, 0x01, 0x95, 0x03, 0x81, 0x03, 0x05,   \
        0x01, 0x15, 0x00, 0x26, 0xAF, 0x04, 0x75, 0x10, 0x55, 0x0E, 0x65, 0x11, 0x09, 0x30, 0x35, 0x00, 0x46, 0xE8,   \
        0x03, 0x95, 0x01, 0x81, 0x02, 0x46, 0xBC, 0x02, 0x26, 0xD3, 0x02, 0x09, 0x31, 0x81, 0x02, 0xC0
constexpr u8 PTP[] = {
    0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, PTP_FINGER, PTP_FINGER,
    0x05, 0x0D, 0x55, 0x00, 0x65, 0x00, 0x09, 0x54, 0x25, 0x7F, 0x95, 0x01, 0x75, 0x08, 0x81, 0x02,
    0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
    0x05, 0x0D, 0x85, 0x02, 0x09, 0x55, 0x09, 0x59, 0x75, 0x04, 0x95, 0x02, 0x25, 0x0F, 0xB1, 0x02,
    0xC0,
    0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x22, 0xA1, 0x02, 0x09, 0x52, 0x15, 0x00,
    0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0, 0x09, 0x22, 0xA1, 0x00, 0x85, 0x04, 0x09,
    0x57, 0x09, 0x58, 0x75, 0x01, 0x95, 0x02, 0x25, 0x01, 0xB1, 0x02, 0x95, 0x06, 0xB1, 0x03, 0xC0,
    0xC0,
};
#undef PTP_FINGER

int g_failures = 0;

void expect(bool ok, const char* device, const char* what) {
    if (!ok) {
        klog::error("touchpad self-test: %s: %s", device, what);
        g_failures++;
    }
}

class FeatureRecorder final : public usb::hid::FeatureIo {
public:
    long get_feature(u8, u8*, u32) override { return -1; }
    long set_feature(u8 id, const u8* buf, u32 len) override {
        if (n < 4) {
            ids[n] = id;
            lens[n] = len;
            memcpy(data[n], buf, len < 4 ? len : 4);
            n++;
        }
        return len;
    }
    u8 ids[4] = {}, data[4][4] = {};
    u32 lens[4] = {};
    int n = 0;
};

void finger(u8* r, bool tip, u8 id, u16 x, u16 y) {
    r[0] = static_cast<u8>((tip ? 3 : 0) | (id << 2));
    r[1] = x & 0xFF, r[2] = x >> 8, r[3] = y & 0xFF, r[4] = y >> 8;
}

void test_ptp() {
    using usb::hid::Touchpad;
    auto* desc = static_cast<usb::hid::ReportDescriptor*>(kmalloc(sizeof(usb::hid::ReportDescriptor)));
    auto* tp = new Touchpad;
    const char* n = "HID precision touchpad";
    expect(desc && usb::hid::parse(PTP, sizeof(PTP), desc) == 0, n, "descriptor parses");
    expect(tp->init(*desc), n, "touch pad application found");
    expect(tp->contacts == 2 && tp->x_max == 1199 && tp->y_max == 723 && tp->units_per_mm == 11 && tp->clickpad &&
               tp->report_id() == 1,
           n, "fingers, range, resolution");
    FeatureRecorder io;
    expect(tp->configure(io), n, "configure");
    expect(io.n == 2 && io.ids[0] == 3 && io.lens[0] == 2 && io.data[0][1] == 3 && io.ids[1] == 4 &&
               io.data[1][1] == 0x03,
           n, "input mode 3, surface + button switches on");

    u8 r[13] = {1};
    Touchpad::Frame f;
    finger(r + 1, true, 0, 600, 300);
    r[11] = 1; // contact count
    expect(tp->process(r, sizeof(r), &f) && f.count == 1 && f.fingers[0].x == 600 && f.fingers[0].y == 423 &&
               !f.button,
           n, "one finger (y flipped to grow upwards)");
    finger(r + 6, true, 1, 700, 400);
    r[11] = 2;
    r[12] = 1; // pad pressed
    expect(tp->process(r, sizeof(r), &f) && f.count == 2 && f.fingers[1].x == 700 && f.button, n,
           "two fingers + click");
    // Hybrid mode: 3 contacts in two reports (count only in the first).
    auto* tp3 = tp; // same layout
    finger(r + 1, true, 0, 100, 100);
    finger(r + 6, true, 1, 200, 200);
    r[11] = 3;
    r[12] = 0;
    expect(!tp3->process(r, sizeof(r), &f), n, "hybrid: first half waits");
    finger(r + 1, true, 2, 300, 300);
    finger(r + 6, false, 0, 0, 0);
    r[11] = 0;
    expect(tp3->process(r, sizeof(r), &f) && f.count == 3 && f.fingers[2].x == 300, n, "hybrid: frame complete");
    finger(r + 1, false, 0, 0, 0);
    r[11] = 0;
    expect(tp->process(r, sizeof(r), &f) && f.count == 0, n, "lift");
    u8 other[3] = {2, 0, 0};
    expect(!tp->process(other, sizeof(other), &f), n, "other report IDs ignored");
    delete tp;
    kfree(desc);
}

void test_synaptics() {
    using namespace ps2pad;
    const char* n = "Synaptics PS/2";
    SynapticsInfo s;
    s.capabilities = (1u << 23) | (1u << 7) | (1u << 1); // extended, pass-through, multifinger
    s.ext_cap = 3u << 12;                                // three extra buttons (the TrackPoint's)
    s.ext_cap_10 = 1u << 16;                             // ... wired to the pad
    Report r;
    const u8 one[6] = {0x94, 0x9B, 0x3C, 0xC0, 0xB8, 0xC4}; // W=6, x=3000, y=2500, z=60
    expect(synaptics_decode(s, one, &r) && r.kind == Report::Touch && r.count == 1 && r.positions == 1 &&
               r.fingers[0].x == 3000 && r.fingers[0].y == 2500 && !r.left,
           n, "one finger");
    const u8 two[6] = {0x80, 0x9B, 0x3C, 0xC0, 0xB8, 0xC4}; // W=0: two fingers
    expect(synaptics_decode(s, two, &r) && r.count == 2 && r.positions == 1, n, "two fingers");
    const u8 lifted[6] = {0x94, 0x00, 0x00, 0xC0, 0x00, 0x00};
    expect(synaptics_decode(s, lifted, &r) && r.kind == Report::Touch && r.count == 0, n, "lift");
    const u8 stick[6] = {0x84, 0x29, 0x00, 0xC4, 0x05, 0xFD}; // W=3: TrackPoint left button, dx +5, dy -3 (up)
    expect(synaptics_decode(s, stick, &r) && r.kind == Report::Stick && r.stick_buttons == 1 && r.dx == 5 &&
               r.dy == 3,
           n, "TrackPoint pass-through");
    const u8 ext[6] = {0x80, 0x00, 0x00, 0xC2, 0x02, 0x00}; // ext-button packet: TrackPoint middle (bit 1)
    expect(synaptics_decode(s, ext, &r) && r.kind == Report::StickButtons && r.stick_buttons == 4, n,
           "TrackPoint buttons on the pad");
    s.ext_cap_0c = 1u << 20; // clickpad: its click is the "middle" bit
    const u8 click[6] = {0x94, 0x9B, 0x3C, 0xC1, 0xB8, 0xC4};
    expect(synaptics_decode(s, click, &r) && r.left && !r.right, n, "clickpad click");
}

void test_elantech() {
    using namespace ps2pad;
    const char* n = "Elantech v4 PS/2";
    ElantechState s;
    s.hw_version = 4;
    s.fw_version = 0x461F02; // IC 6, clickpad bit (0x1000) set
    s.buttonpad = true;
    s.trackpoint = true;
    Report r;
    const u8 head[6] = {0x00, 0x03, 0xE8, 0x31, 0x01, 0xF4}; // finger 0 at 1000, 500
    expect(elantech_decode(s, head, &r) && r.kind == Report::Touch && r.count == 1 && r.fingers[0].x == 1000 &&
               r.fingers[0].y == 500,
           n, "head packet");
    const u8 motion[6] = {0x20, 0x0A, 0xFB, 0x12, 0x00, 0x00}; // finger 0 moves +10, -5
    expect(elantech_decode(s, motion, &r) && r.count == 1 && r.fingers[0].x == 1010 && r.fingers[0].y == 495, n,
           "motion packet");
    const u8 head2[6] = {0x01, 0x05, 0x00, 0x51, 0x02, 0x00}; // finger 1 at 1280, 512; pad pressed
    expect(elantech_decode(s, head2, &r) && r.count == 2 && r.left, n, "second finger + click");
    const u8 status[6] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00}; // nobody touching
    expect(elantech_decode(s, status, &r) && r.count == 0, n, "status packet: lift");
    const u8 bad[6] = {0x08, 0x00, 0x00, 0x00, 0x00, 0x00};
    expect(!elantech_decode(s, bad, &r), n, "out-of-sync bytes rejected");
    const u8 stick[6] = {0x10, 0x00, 0x80, 0x26, 0xFB, 0x03}; // TrackPoint: dx -5, dy -3
    expect(elantech_decode(s, stick, &r) && r.kind == Report::Stick && r.dx == -5 && r.dy == -3, n, "TrackPoint");
}

} // namespace

void run_touchpad_selftest() {
    g_failures = 0;
    test_ptp();
    test_synaptics();
    test_elantech();
    if (g_failures)
        panic("touchpad self-test: %d check(s) failed", g_failures);
    klog::ok("Touchpad self-test: HID precision touchpad (incl. hybrid reports), Synaptics + TrackPoint, Elantech v4");
}

} // namespace input
