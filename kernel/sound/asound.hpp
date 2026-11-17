#pragma once

#include <lib/types.hpp>

// The Linux ALSA user-space ABI (include/uapi/sound/asound.h), x86_64 layout.
// libasound talks to /dev/snd/* exclusively through these structures and
// ioctl numbers, so they must match Linux byte for byte (the static_asserts
// pin the sizes measured against the real header).
namespace sound::abi {

constexpr u32 protocol_version(u32 major, u32 minor, u32 sub) { return (major << 16) | (minor << 8) | sub; }

// We report slightly older protocol versions than current Linux on purpose:
// 2.0.15 (PCM) predates the subformat-msbits extension (2.0.17) and the
// status64/"new" mmap offsets, so libasound uses the simplest code paths.
constexpr u32 PCM_VERSION = protocol_version(2, 0, 15);
constexpr u32 CTL_VERSION = protocol_version(2, 0, 9);
constexpr u32 TIMER_VERSION = protocol_version(2, 0, 7);

// ---- ioctl encoding (asm-generic/ioctl.h) ----
constexpr u32 ioc_nr(u64 req) { return req & 0xFF; }
constexpr u32 ioc_type(u64 req) { return (req >> 8) & 0xFF; }
constexpr u32 ioc_size(u64 req) { return (req >> 16) & 0x3FFF; }

// ---- PCM ----
enum : int { STREAM_PLAYBACK = 0, STREAM_CAPTURE = 1 };

enum : int {
    ACCESS_MMAP_INTERLEAVED = 0, ACCESS_MMAP_NONINTERLEAVED = 1, ACCESS_MMAP_COMPLEX = 2,
    ACCESS_RW_INTERLEAVED = 3, ACCESS_RW_NONINTERLEAVED = 4,
};

enum : int {
    FORMAT_S8 = 0, FORMAT_U8 = 1, FORMAT_S16_LE = 2, FORMAT_S16_BE = 3, FORMAT_U16_LE = 4, FORMAT_U16_BE = 5,
    FORMAT_S24_LE = 6, FORMAT_S24_BE = 7, FORMAT_U24_LE = 8, FORMAT_U24_BE = 9,
    FORMAT_S32_LE = 10, FORMAT_S32_BE = 11, FORMAT_U32_LE = 12, FORMAT_U32_BE = 13,
    FORMAT_FLOAT_LE = 14, FORMAT_FLOAT_BE = 15, FORMAT_FLOAT64_LE = 16, FORMAT_FLOAT64_BE = 17,
    FORMAT_S20_LE = 25, FORMAT_S24_3LE = 32, FORMAT_S20_3LE = 36,
    FORMAT_LAST = 52,
};

constexpr u32 SUBFORMAT_STD = 0;

enum : u32 {
    INFO_MMAP = 0x1, INFO_MMAP_VALID = 0x2, INFO_DOUBLE = 0x4, INFO_BATCH = 0x10,
    INFO_INTERLEAVED = 0x100, INFO_NONINTERLEAVED = 0x200, INFO_BLOCK_TRANSFER = 0x10000,
    INFO_RESUME = 0x40000, INFO_PAUSE = 0x80000, INFO_HALF_DUPLEX = 0x100000, INFO_JOINT_DUPLEX = 0x200000,
    INFO_SYNC_START = 0x400000, INFO_NO_PERIOD_WAKEUP = 0x800000,
    INFO_HAS_LINK_ATIME = 0x1000000,
};

enum : int {
    STATE_OPEN = 0, STATE_SETUP = 1, STATE_PREPARED = 2, STATE_RUNNING = 3, STATE_XRUN = 4,
    STATE_DRAINING = 5, STATE_PAUSED = 6, STATE_SUSPENDED = 7, STATE_DISCONNECTED = 8,
};

// mmap offsets of the status/control pages (64-bit: "old" and "new" are the same layout).
constexpr u64 MMAP_OFFSET_DATA = 0x00000000;
constexpr u64 MMAP_OFFSET_STATUS_OLD = 0x80000000, MMAP_OFFSET_CONTROL_OLD = 0x81000000;
constexpr u64 MMAP_OFFSET_STATUS_NEW = 0x82000000, MMAP_OFFSET_CONTROL_NEW = 0x83000000;

struct PcmInfo {
    u32 device, subdevice;
    i32 stream, card;
    u8 id[64];
    u8 name[80];
    u8 subname[32];
    i32 dev_class, dev_subclass;
    u32 subdevices_count, subdevices_avail;
    u8 pad1[16];
    u8 reserved[64];
};
static_assert(sizeof(PcmInfo) == 288);

// hw_params parameter indices: three masks, then twelve intervals.
enum : int {
    PARAM_ACCESS = 0, PARAM_FORMAT = 1, PARAM_SUBFORMAT = 2,
    PARAM_SAMPLE_BITS = 8, PARAM_FRAME_BITS = 9, PARAM_CHANNELS = 10, PARAM_RATE = 11,
    PARAM_PERIOD_TIME = 12, PARAM_PERIOD_SIZE = 13, PARAM_PERIOD_BYTES = 14, PARAM_PERIODS = 15,
    PARAM_BUFFER_TIME = 16, PARAM_BUFFER_SIZE = 17, PARAM_BUFFER_BYTES = 18, PARAM_TICK_TIME = 19,
    PARAM_FIRST_MASK = PARAM_ACCESS, PARAM_LAST_MASK = PARAM_SUBFORMAT,
    PARAM_FIRST_INTERVAL = PARAM_SAMPLE_BITS, PARAM_LAST_INTERVAL = PARAM_TICK_TIME,
};
constexpr int MASK_COUNT = PARAM_LAST_MASK - PARAM_FIRST_MASK + 1;
constexpr int INTERVAL_COUNT = PARAM_LAST_INTERVAL - PARAM_FIRST_INTERVAL + 1;

struct Interval {
    u32 min, max;
    u32 openmin : 1, openmax : 1, integer : 1, empty : 1;
};
static_assert(sizeof(Interval) == 12);

struct Mask {
    u32 bits[8]; // 256 bits
};

struct HwParams {
    u32 flags;
    Mask masks[MASK_COUNT];
    Mask mres[5];
    Interval intervals[INTERVAL_COUNT];
    Interval ires[9];
    u32 rmask, cmask, info, msbits, rate_num, rate_den;
    u64 fifo_size;
    u8 sync[16];
    u8 reserved[48];
};
static_assert(sizeof(HwParams) == 608);
static_assert(__builtin_offsetof(HwParams, intervals) == 260);
static_assert(__builtin_offsetof(HwParams, rmask) == 512);

struct SwParams {
    i32 tstamp_mode;
    u32 period_step, sleep_min;
    u64 avail_min, xfer_align, start_threshold, stop_threshold, silence_threshold, silence_size, boundary;
    u32 proto, tstamp_type;
    u8 reserved[56];
};
static_assert(sizeof(SwParams) == 136);

struct Timespec {
    i64 sec, nsec;
};

struct PcmStatus {
    i32 state;
    u32 pad1;
    Timespec trigger_tstamp;
    Timespec tstamp;
    u64 appl_ptr, hw_ptr;
    i64 delay;
    u64 avail, avail_max, overrange;
    i32 suspended_state;
    u32 audio_tstamp_data;
    Timespec audio_tstamp;
    Timespec driver_tstamp;
    u32 audio_tstamp_accuracy;
    u8 reserved[20];
};
static_assert(sizeof(PcmStatus) == 152);

struct MmapStatus {
    i32 state;
    i32 pad1;
    u64 hw_ptr;
    Timespec tstamp;
    i32 suspended_state;
    i32 pad3;
    Timespec audio_tstamp;
};
static_assert(sizeof(MmapStatus) == 56);

struct MmapControl {
    u64 appl_ptr;
    u64 avail_min;
};

constexpr u32 SYNC_PTR_HWSYNC = 1, SYNC_PTR_APPL = 2, SYNC_PTR_AVAIL_MIN = 4;

struct SyncPtr {
    u32 flags;
    u32 pad;
    union {
        MmapStatus status;
        u8 reserved[64];
    } s;
    union {
        MmapControl control;
        u8 reserved[64];
    } c;
};
static_assert(sizeof(SyncPtr) == 136);

struct Xferi {
    i64 result;
    u64 buf;     // user pointer
    u64 frames;
};

struct Xfern {
    i64 result;
    u64 bufs;    // user pointer to an array of per-channel user pointers
    u64 frames;
};

struct ChannelInfo {
    u32 channel;
    u32 pad;
    i64 offset;  // mmap offset
    u32 first, step; // in bits
};
static_assert(sizeof(ChannelInfo) == 24);

// PCM ioctl numbers ('A').
enum : u32 {
    PCM_PVERSION = 0x00, PCM_INFO = 0x01, PCM_TSTAMP = 0x02, PCM_TTSTAMP = 0x03, PCM_USER_PVERSION = 0x04,
    PCM_HW_REFINE = 0x10, PCM_HW_PARAMS = 0x11, PCM_HW_FREE = 0x12, PCM_SW_PARAMS = 0x13,
    PCM_STATUS = 0x20, PCM_DELAY = 0x21, PCM_HWSYNC = 0x22, PCM_SYNC_PTR = 0x23, PCM_STATUS_EXT = 0x24,
    PCM_CHANNEL_INFO = 0x32,
    PCM_PREPARE = 0x40, PCM_RESET = 0x41, PCM_START = 0x42, PCM_DROP = 0x43, PCM_DRAIN = 0x44,
    PCM_PAUSE = 0x45, PCM_REWIND = 0x46, PCM_RESUME = 0x47, PCM_XRUN = 0x48, PCM_FORWARD = 0x49,
    PCM_WRITEI_FRAMES = 0x50, PCM_READI_FRAMES = 0x51, PCM_WRITEN_FRAMES = 0x52, PCM_READN_FRAMES = 0x53,
    PCM_LINK = 0x60, PCM_UNLINK = 0x61,
};

// ---- Control ----
struct CtlCardInfo {
    i32 card;
    i32 pad;
    u8 id[16];
    u8 driver[16];
    u8 name[32];
    u8 longname[80];
    u8 reserved_[16];
    u8 mixername[80];
    u8 components[128];
};
static_assert(sizeof(CtlCardInfo) == 376);

enum : i32 {
    ELEM_TYPE_NONE = 0, ELEM_TYPE_BOOLEAN = 1, ELEM_TYPE_INTEGER = 2, ELEM_TYPE_ENUMERATED = 3,
    ELEM_TYPE_BYTES = 4, ELEM_TYPE_IEC958 = 5, ELEM_TYPE_INTEGER64 = 6,
};
enum : i32 { IFACE_CARD = 0, IFACE_HWDEP = 1, IFACE_MIXER = 2, IFACE_PCM = 3 };
enum : u32 {
    ACCESS_READ = 1, ACCESS_WRITE = 2, ACCESS_READWRITE = 3, ACCESS_VOLATILE = 4,
    ACCESS_TLV_READ = 0x10, ACCESS_INACTIVE = 0x100,
};

struct ElemId {
    u32 numid;
    i32 iface;
    u32 device, subdevice;
    u8 name[44];
    u32 index;
};
static_assert(sizeof(ElemId) == 64);

struct ElemList {
    u32 offset, space, used, count;
    u64 pids; // user pointer to ElemId[space]
    u8 reserved[50];
};
static_assert(sizeof(ElemList) == 80);

struct ElemInfo {
    ElemId id;
    i32 type;
    u32 access;
    u32 count;
    i32 owner;
    union {
        struct {
            i64 min, max, step;
        } integer;
        struct {
            i64 min, max, step;
        } integer64;
        struct {
            u32 items, item;
            char name[64];
            u64 names_ptr;
            u32 names_length;
        } enumerated;
        u8 reserved[128];
    } value;
    u8 reserved[64];
};
static_assert(sizeof(ElemInfo) == 272);
static_assert(__builtin_offsetof(ElemInfo, value) == 80);

struct ElemValue {
    ElemId id;
    u32 indirect; // bit field in C; always 0 today
    u32 pad;
    union {
        i64 integer[128];
        i64 integer64[64];
        u32 enumerated[128];
        u8 bytes[512];
    } value;
    u8 reserved_tail[1224 - 72 - 1024];
};
static_assert(sizeof(ElemValue) == 1224);
static_assert(__builtin_offsetof(ElemValue, value) == 72);

struct CtlTlv {
    u32 numid;
    u32 length; // bytes of tlv[] the caller provides / we fill
    // u32 tlv[] follows
};

constexpr u32 TLVT_DB_SCALE = 1;
constexpr u32 TLV_DB_SCALE_MUTE = 0x10000;

constexpr u32 CTL_EVENT_MASK_VALUE = 1, CTL_EVENT_MASK_INFO = 2, CTL_EVENT_MASK_ADD = 4;

struct CtlEvent {
    i32 type; // 0 = element event
    u32 mask;
    ElemId id;
    u8 pad[72 - 8 - 64];
};
static_assert(sizeof(CtlEvent) == 72);

// Control ioctl numbers ('U').
enum : u32 {
    CTL_PVERSION = 0x00, CTL_CARD_INFO = 0x01, CTL_ELEM_LIST = 0x10, CTL_ELEM_INFO = 0x11, CTL_ELEM_READ = 0x12,
    CTL_ELEM_WRITE = 0x13, CTL_ELEM_LOCK = 0x14, CTL_ELEM_UNLOCK = 0x15, CTL_SUBSCRIBE_EVENTS = 0x16,
    CTL_ELEM_ADD = 0x17, CTL_ELEM_REPLACE = 0x18, CTL_ELEM_REMOVE = 0x19,
    CTL_TLV_READ = 0x1a, CTL_TLV_WRITE = 0x1b, CTL_TLV_COMMAND = 0x1c,
    CTL_HWDEP_NEXT_DEVICE = 0x20, CTL_HWDEP_INFO = 0x21,
    CTL_PCM_NEXT_DEVICE = 0x30, CTL_PCM_INFO = 0x31, CTL_PCM_PREFER_SUBDEVICE = 0x32,
    CTL_RAWMIDI_NEXT_DEVICE = 0x40, CTL_RAWMIDI_INFO = 0x41, CTL_RAWMIDI_PREFER_SUBDEVICE = 0x42,
    CTL_UMP_NEXT_DEVICE = 0x43,
    CTL_POWER = 0xd0, CTL_POWER_STATE = 0xd1,
};

// ---- Timer ('T') ----
struct TimerId {
    i32 dev_class, dev_sclass, card, device, subdevice;
};
static_assert(sizeof(TimerId) == 20);

enum : u32 { TIMER_PVERSION = 0x00, TIMER_NEXT_DEVICE = 0x01, TIMER_TREAD = 0x02, TIMER_GINFO = 0x03 };

} // namespace sound::abi
