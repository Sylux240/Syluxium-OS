#include <arch/x86_64/cpu.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace rtc {

namespace {

u64 g_boot_epoch_ns = 0;
u64 g_boot_mono_ns = 0;

u8 cmos_read(u8 reg) {
    arch::outb(0x70, reg | 0x80); // bit 7: keep NMIs disabled while selecting
    return arch::inb(0x71);
}

bool update_in_progress() { return cmos_read(0x0A) & 0x80; }

struct RawTime {
    u8 sec, min, hour, day, month, year, century;
    bool operator==(const RawTime&) const = default;
};

RawTime read_raw() {
    while (update_in_progress())
        arch::pause();
    return RawTime{cmos_read(0x00), cmos_read(0x02), cmos_read(0x04), cmos_read(0x07),
                   cmos_read(0x08), cmos_read(0x09), cmos_read(0x32)};
}

u8 from_bcd(u8 v) { return (v & 0x0F) + (v >> 4) * 10; }

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
i64 days_from_civil(i64 y, u32 m, u32 d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = static_cast<u32>(y - era * 400);
    u32 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<i64>(doe) - 719468;
}

} // namespace

void init() {
    // Read until two consecutive reads agree (the RTC may tick mid-read).
    RawTime a = read_raw(), b;
    do {
        b = a;
        a = read_raw();
    } while (!(a == b));

    u8 status_b = cmos_read(0x0B);
    bool bcd = !(status_b & 0x04);
    bool h12 = !(status_b & 0x02);

    u32 sec = a.sec, min = a.min, day = a.day, month = a.month, year = a.year, century = a.century;
    u32 hour = a.hour & 0x7F;
    bool pm = a.hour & 0x80;
    if (bcd) {
        sec = from_bcd(sec); min = from_bcd(min); hour = from_bcd(hour);
        day = from_bcd(day); month = from_bcd(month); year = from_bcd(year);
        century = from_bcd(century);
    }
    if (h12) {
        hour %= 12;
        if (pm) hour += 12;
    }
    u32 full_year = (century >= 19 && century <= 30) ? century * 100 + year : 2000 + year;

    i64 days = days_from_civil(full_year, month, day);
    u64 epoch = static_cast<u64>(days) * 86400 + hour * 3600 + min * 60 + sec;
    g_boot_epoch_ns = epoch * 1'000'000'000;
    g_boot_mono_ns = clock::now_ns();
}

u64 boot_epoch_seconds() { return g_boot_epoch_ns / 1'000'000'000; }

u64 realtime_ns() { return g_boot_epoch_ns + (clock::now_ns() - g_boot_mono_ns); }

} // namespace rtc
