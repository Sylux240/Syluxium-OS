// ALSA kernel-interface test: talks to /dev/snd/* with raw ioctls (no
// libasound), so it exercises exactly the ABI the kernel implements.
//
//   sndtest info                      card info, mixer elements, PCM devices
//   sndtest wav FILE HZ SECS [RATE]   write a stereo S16_LE sine-wave WAV file
//   sndtest play HZ SECS              play a sine with HW_PARAMS/WRITEI/DRAIN
//   sndtest abi                       state machine checks (refine, EAGAIN, xrun, pause, mmap, rewind)
//   sndtest capture SECS              record and report peak/RMS levels
// Exit status 0 = all checks passed.
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  [PASS] "); } else { printf("  [FAIL] "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static struct snd_interval* iv(struct snd_pcm_hw_params* p, int k) { return &p->intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }
static struct snd_mask* mk(struct snd_pcm_hw_params* p, int k) { return &p->masks[k - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }

static void params_any(struct snd_pcm_hw_params* p) {
    memset(p, 0, sizeof(*p));
    for (int k = 0; k < 3; k++) memset(&p->masks[k], 0xff, sizeof(p->masks[k]));
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++) {
        iv(p, k)->min = 0; iv(p, k)->max = ~0u;
    }
    p->rmask = ~0u;
}
static void set_mask(struct snd_pcm_hw_params* p, int k, int bit) {
    memset(mk(p, k), 0, sizeof(struct snd_mask));
    mk(p, k)->bits[bit / 32] = 1u << (bit % 32);
}
static void set_iv(struct snd_pcm_hw_params* p, int k, unsigned v) {
    iv(p, k)->min = iv(p, k)->max = v; iv(p, k)->integer = 1;
}

// Configure: stereo S16_LE at `rate`, `period` frames, `periods` periods.
static int configure(int fd, int access, unsigned rate, unsigned period, unsigned periods) {
    struct snd_pcm_hw_params p;
    params_any(&p);
    set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, access);
    set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    set_iv(&p, SNDRV_PCM_HW_PARAM_RATE, rate);
    set_iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
    set_iv(&p, SNDRV_PCM_HW_PARAM_PERIODS, periods);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0) { perror("HW_PARAMS"); return -1; }
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof(sw));
    sw.avail_min = period;
    sw.start_threshold = period * periods;
    sw.stop_threshold = period * periods;
    sw.boundary = 0;
    sw.proto = SNDRV_PCM_VERSION;
    sw.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) { perror("SW_PARAMS"); return -1; }
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) { perror("PREPARE"); return -1; }
    return 0;
}

static void sine(int16_t* buf, size_t frames, double hz, unsigned rate, size_t* phase, double amp) {
    for (size_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(amp * 32767.0 * sin(2 * M_PI * hz * (double)(*phase)++ / rate));
        buf[2 * i] = buf[2 * i + 1] = v;
    }
}

static int pcm_state(int fd) {
    struct snd_pcm_status st;
    memset(&st, 0, sizeof(st));
    if (ioctl(fd, SNDRV_PCM_IOCTL_STATUS, &st) < 0) return -errno;
    return st.state;
}

static int cmd_info(void) {
    int fd = open("/dev/snd/controlC0", O_RDWR);
    if (fd < 0) { perror("/dev/snd/controlC0"); return 1; }
    int ver = 0;
    CHECK(ioctl(fd, SNDRV_CTL_IOCTL_PVERSION, &ver) == 0, "control protocol %d.%d.%d", ver >> 16, (ver >> 8) & 255, ver & 255);
    struct snd_ctl_card_info ci;
    CHECK(ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci) == 0, "card %d: id '%s' driver '%s' name '%s' mixer '%s'", ci.card, ci.id, ci.driver, ci.name, ci.mixername);
    printf("        longname: %s\n", ci.longname);
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof(list));
    ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list);
    struct snd_ctl_elem_id ids[64];
    list.space = list.count < 64 ? list.count : 64;
    list.pids = ids;
    CHECK(ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0 && list.used == list.space, "%u mixer elements", list.count);
    for (unsigned i = 0; i < list.used; i++) {
        struct snd_ctl_elem_info info;
        memset(&info, 0, sizeof(info));
        info.id = ids[i];
        ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info);
        struct snd_ctl_elem_value v;
        memset(&v, 0, sizeof(v));
        v.id = ids[i];
        ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v);
        printf("        numid=%u '%s' type=%d count=%u range %ld..%ld value %ld,%ld\n", ids[i].numid, ids[i].name, info.type,
               info.count, info.value.integer.min, info.value.integer.max, v.value.integer.value[0], v.value.integer.value[1]);
    }
    int dev = -1;
    while (ioctl(fd, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE, &dev) == 0 && dev >= 0) {
        for (int stream = 0; stream < 2; stream++) {
            struct snd_pcm_info pi;
            memset(&pi, 0, sizeof(pi));
            pi.device = dev; pi.stream = stream;
            if (ioctl(fd, SNDRV_CTL_IOCTL_PCM_INFO, &pi) == 0)
                printf("        pcm %d %s: '%s' (%s)\n", dev, stream ? "capture" : "playback", pi.name, pi.subname);
        }
    }
    close(fd);
    return failures != 0;
}

static int cmd_wav(const char* path, double hz, double secs, unsigned rate) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    uint32_t frames = (uint32_t)(secs * rate), data = frames * 4;
    uint32_t hdr[11] = {0x46464952, 36 + data, 0x45564157, 0x20746d66, 16, 0x00020001, rate, rate * 4, 0x00100004, 0x61746164, data};
    fwrite(hdr, 4, 11, f);
    size_t phase = 0;
    int16_t buf[2048];
    for (uint32_t done = 0; done < frames;) {
        uint32_t n = frames - done < 1024 ? frames - done : 1024;
        sine(buf, n, hz, rate, &phase, 0.5);
        fwrite(buf, 4, n, f);
        done += n;
    }
    fclose(f);
    printf("wrote %s: %u frames, %.0f Hz, %u Hz stereo S16_LE\n", path, frames, hz, rate);
    return 0;
}

static int cmd_play(double hz, double secs) {
    int fd = open("/dev/snd/pcmC0D0p", O_RDWR);
    if (fd < 0) { perror("pcmC0D0p"); return 1; }
    const unsigned rate = 48000, period = 1024, periods = 4;
    if (configure(fd, SNDRV_PCM_ACCESS_RW_INTERLEAVED, rate, period, periods) < 0) return 1;
    size_t phase = 0, total = (size_t)(secs * rate);
    int16_t buf[period * 2];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (phase < total) {
        size_t n = total - phase < period ? total - phase : period;
        sine(buf, n, hz, rate, &phase, 0.5);
        struct snd_xferi x = {0, buf, n};
        if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (errno == EPIPE) { printf("underrun, recovering\n"); ioctl(fd, SNDRV_PCM_IOCTL_PREPARE); continue; }
            perror("WRITEI"); return 1;
        }
    }
    int r = ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    CHECK(r == 0, "played %.1f s of %.0f Hz in %.2f s (drain ok, state %d)", secs, hz, el, pcm_state(fd));
    close(fd);
    return failures != 0;
}

static int cmd_abi(void) {
    int fd = open("/dev/snd/pcmC0D0p", O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("pcmC0D0p"); return 1; }
    int fd2 = open("/dev/snd/pcmC0D0p", O_RDWR);
    int ver;
    CHECK(fd2 >= 0 && ioctl(fd2, SNDRV_PCM_IOCTL_PVERSION, &ver) < 0 && errno == EBUSY, "second opener gets EBUSY");
    if (fd2 >= 0) close(fd2);
    CHECK(ioctl(fd, SNDRV_PCM_IOCTL_PVERSION, &ver) == 0, "PCM protocol %d.%d.%d", ver >> 16, (ver >> 8) & 255, ver & 255);
    struct snd_pcm_info pi;
    CHECK(ioctl(fd, SNDRV_PCM_IOCTL_INFO, &pi) == 0, "INFO: '%s' card %d device %u", pi.name, pi.card, pi.device);

    struct snd_pcm_hw_params p;
    params_any(&p);
    CHECK(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) == 0, "HW_REFINE(any): channels %u-%u rate %u-%u period_bytes %u-%u periods %u-%u buffer_bytes %u-%u formats 0x%x",
          iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS)->min, iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS)->max,
          iv(&p, SNDRV_PCM_HW_PARAM_RATE)->min, iv(&p, SNDRV_PCM_HW_PARAM_RATE)->max,
          iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->min, iv(&p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->max,
          iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->min, iv(&p, SNDRV_PCM_HW_PARAM_PERIODS)->max,
          iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->min, iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->max,
          mk(&p, SNDRV_PCM_HW_PARAM_FORMAT)->bits[0]);
    params_any(&p);
    set_iv(&p, SNDRV_PCM_HW_PARAM_RATE, 12345);
    CHECK(ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0 && errno == EINVAL, "HW_REFINE rejects 12345 Hz");
    params_any(&p);
    set_iv(&p, SNDRV_PCM_HW_PARAM_RATE, 48000);
    set_iv(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_TIME, 100000);
    iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_TIME)->integer = 0;
    int r = ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p);
    CHECK(r == 0 && iv(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS)->min == 32 && iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min == 4800 &&
          iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->max == 4800,
          "HW_REFINE derives frame_bits=%u, buffer_size=%u-%u from 100 ms @ 48 kHz",
          iv(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS)->min, iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min, iv(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->max);

    CHECK(configure(fd, SNDRV_PCM_ACCESS_MMAP_INTERLEAVED, 48000, 1024, 4) == 0, "HW_PARAMS mmap 48 kHz, 4 x 1024 frames, PREPARE");
    long ps = sysconf(_SC_PAGESIZE);
    volatile struct snd_pcm_mmap_status* status = mmap(NULL, ps, PROT_READ, MAP_SHARED, fd, SNDRV_PCM_MMAP_OFFSET_STATUS);
    volatile struct snd_pcm_mmap_control* control = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SNDRV_PCM_MMAP_OFFSET_CONTROL);
    int16_t* data = mmap(NULL, 4096 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(status != MAP_FAILED && control != MAP_FAILED && data != MAP_FAILED, "mmap status/control/data");
    if (status != MAP_FAILED && control != MAP_FAILED && data != MAP_FAILED) {
        CHECK(status->state == SNDRV_PCM_STATE_PREPARED && control->avail_min == 1024, "mmapped status says PREPARED, avail_min 1024");
        size_t phase = 0;
        sine(data, 4096, 660, 48000, &phase, 0.3);
        control->appl_ptr = 4096; // commit a full buffer through the control page
        CHECK(ioctl(fd, SNDRV_PCM_IOCTL_START) == 0, "START after mmap commit");
        usleep(30000);
        ioctl(fd, SNDRV_PCM_IOCTL_HWSYNC);
        CHECK(status->state == SNDRV_PCM_STATE_RUNNING && status->hw_ptr > 0, "running, hw_ptr advances (%lu)", (unsigned long)status->hw_ptr);
        CHECK(ioctl(fd, SNDRV_PCM_IOCTL_PAUSE, 1) == 0 && status->state == SNDRV_PCM_STATE_PAUSED, "PAUSE");
        unsigned long hw = status->hw_ptr;
        usleep(30000);
        ioctl(fd, SNDRV_PCM_IOCTL_HWSYNC);
        CHECK(status->hw_ptr == hw, "hw_ptr frozen while paused");
        CHECK(ioctl(fd, SNDRV_PCM_IOCTL_PAUSE, 0) == 0 && status->state == SNDRV_PCM_STATE_RUNNING, "resume");
        snd_pcm_uframes_t back = 512;
        CHECK(ioctl(fd, SNDRV_PCM_IOCTL_REWIND, &back) == 0 && back > 0, "REWIND %lu frames", (unsigned long)back);
        usleep(250000); // let it run dry
        CHECK(pcm_state(fd) == SNDRV_PCM_STATE_XRUN, "underrun detected (state XRUN)");
        snd_pcm_sframes_t delay;
        CHECK(ioctl(fd, SNDRV_PCM_IOCTL_DELAY, &delay) < 0 && errno == EPIPE, "DELAY fails with EPIPE in XRUN");
        munmap((void*)status, ps); munmap((void*)control, ps); munmap(data, 4096 * 4);
    }
    ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE);

    // Read/write, non-blocking.
    CHECK(configure(fd, SNDRV_PCM_ACCESS_RW_INTERLEAVED, 48000, 1024, 4) == 0, "HW_PARAMS rw");
    int16_t buf[1024 * 2] = {0};
    int written = 0;
    for (int i = 0; i < 8; i++) {
        struct snd_xferi x = {0, buf, 1024};
        if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            CHECK(errno == EAGAIN && written == 4, "non-blocking write: %d periods fit, then EAGAIN", written);
            break;
        }
        written++;
    }
    CHECK(pcm_state(fd) == SNDRV_PCM_STATE_RUNNING, "auto-start at start_threshold");
    struct pollfd pfd = {fd, POLLOUT, 0};
    CHECK(poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLOUT), "poll() wakes up when a period is free");
    struct snd_pcm_sync_ptr sp;
    memset(&sp, 0, sizeof(sp));
    sp.flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN | SNDRV_PCM_SYNC_PTR_HWSYNC;
    CHECK(ioctl(fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sp) == 0 && sp.c.control.appl_ptr == 4096, "SYNC_PTR: appl_ptr %lu hw_ptr %lu",
          (unsigned long)sp.c.control.appl_ptr, (unsigned long)sp.s.status.hw_ptr);
    CHECK(ioctl(fd, SNDRV_PCM_IOCTL_DROP) == 0 && pcm_state(fd) == SNDRV_PCM_STATE_SETUP, "DROP -> SETUP");
    close(fd);
    return failures != 0;
}

static int cmd_capture(double secs) {
    int fd = open("/dev/snd/pcmC0D0c", O_RDWR);
    if (fd < 0) { perror("pcmC0D0c"); return 1; }
    if (configure(fd, SNDRV_PCM_ACCESS_RW_INTERLEAVED, 48000, 1024, 4) < 0) return 1;
    size_t total = (size_t)(secs * 48000), got = 0;
    int16_t buf[1024 * 2];
    double sum = 0; int peak = 0;
    while (got < total) {
        struct snd_xferi x = {0, buf, 1024};
        if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) < 0) {
            if (errno == EPIPE) { ioctl(fd, SNDRV_PCM_IOCTL_PREPARE); continue; }
            perror("READI"); return 1;
        }
        for (long i = 0; i < x.result * 2; i++) { int v = abs(buf[i]); if (v > peak) peak = v; sum += (double)buf[i] * buf[i]; }
        got += x.result;
    }
    CHECK(got >= total, "captured %zu frames, peak %d, rms %.1f", got, peak, sqrt(sum / (got * 2)));
    close(fd);
    return failures != 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "info")) return cmd_info();
    if (argc >= 5 && !strcmp(argv[1], "wav")) return cmd_wav(argv[2], atof(argv[3]), atof(argv[4]), argc > 5 ? atoi(argv[5]) : 48000);
    if (argc >= 4 && !strcmp(argv[1], "play")) return cmd_play(atof(argv[2]), atof(argv[3]));
    if (argc >= 2 && !strcmp(argv[1], "abi")) { int r = cmd_abi(); printf("sndtest abi: %d failure(s)\n", failures); return r; }
    if (argc >= 3 && !strcmp(argv[1], "capture")) return cmd_capture(atof(argv[2]));
    fprintf(stderr, "usage: sndtest info | wav FILE HZ SECS [RATE] | play HZ SECS | abi | capture SECS\n");
    return 2;
}
