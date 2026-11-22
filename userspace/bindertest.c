// Binder IPC between two processes, the way Android's libbinder talks to the
// kernel (no libbinder here, just the ioctls):
//   1. the server becomes the context manager (handle 0) of a binder device
//   2. the client sends 1000 transactions to handle 0, each answered
//   3. one transaction carries a memfd: the server reads it through its own
//      descriptor (binder installs it in the receiving process)
//   4. binderfs: mount -t binder, BINDER_CTL_ADD a device, the same ping-pong
//      on it
//   bindertest [device]      (default /dev/binder, then a binderfs instance)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAP_SIZE (128 * 1024)
#define CODE_PING 1
#define CODE_FD 2
#define CODE_QUIT 3
#define ROUNDS 1000

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("bindertest: FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

struct conn {
    int fd;
    void* map;
};

static int conn_open(struct conn* c, const char* dev) {
    c->fd = open(dev, O_RDWR | O_CLOEXEC);
    if (c->fd < 0) {
        printf("bindertest: open %s: %s\n", dev, strerror(errno));
        return -1;
    }
    struct binder_version v = {0};
    if (ioctl(c->fd, BINDER_VERSION, &v) < 0 || v.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION) {
        printf("bindertest: BINDER_VERSION: %s (protocol %d)\n", strerror(errno), v.protocol_version);
        return -1;
    }
    c->map = mmap(NULL, MAP_SIZE, PROT_READ, MAP_PRIVATE, c->fd, 0);
    if (c->map == MAP_FAILED) {
        printf("bindertest: mmap: %s\n", strerror(errno));
        return -1;
    }
    uint32_t max_threads = 0;
    ioctl(c->fd, BINDER_SET_MAX_THREADS, &max_threads);
    return 0;
}

// One BINDER_WRITE_READ: write `wbuf`, read into `rbuf` (blocking).
static int write_read(struct conn* c, const void* wbuf, size_t wlen, void* rbuf, size_t rlen, size_t* got) {
    struct binder_write_read bwr = {
        .write_size = wlen, .write_buffer = (uintptr_t)wbuf,
        .read_size = rlen, .read_buffer = (uintptr_t)rbuf,
    };
    int r;
    do
        r = ioctl(c->fd, BINDER_WRITE_READ, &bwr);
    while (r < 0 && errno == EINTR);
    if (r < 0) {
        printf("bindertest: BINDER_WRITE_READ: %s\n", strerror(errno));
        return -1;
    }
    if (got)
        *got = bwr.read_consumed;
    return 0;
}

// ---------------------------------------------------------------- server

static void server_reply(struct conn* c, const void* buffer, const void* data, size_t len, int send_fd_reply) {
    struct {
        uint32_t free_cmd;
        binder_uintptr_t free_ptr;
        uint32_t reply_cmd;
        struct binder_transaction_data tr;
    } __attribute__((packed)) w = {0};
    w.free_cmd = BC_FREE_BUFFER;
    w.free_ptr = (uintptr_t)buffer;
    w.reply_cmd = BC_REPLY;
    w.tr.data_size = len;
    w.tr.data.ptr.buffer = (uintptr_t)data;
    (void)send_fd_reply;
    write_read(c, &w, sizeof(w), NULL, 0, NULL);
}

static int server(const char* dev, int ready_fd) {
    struct conn c;
    if (conn_open(&c, dev) < 0)
        return 1;
    // As Android's servicemanager: accepts descriptors, wants the caller's
    // security context with every call.
    struct flat_binder_object mgr = {.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS | FLAT_BINDER_FLAG_TXN_SECURITY_CTX};
    if (ioctl(c.fd, BINDER_SET_CONTEXT_MGR_EXT, &mgr) < 0) {
        printf("bindertest: BINDER_SET_CONTEXT_MGR_EXT: %s\n", strerror(errno));
        return 1;
    }
    uint32_t enter = BC_ENTER_LOOPER;
    write_read(&c, &enter, sizeof(enter), NULL, 0, NULL);
    char ok = 1;
    if (write(ready_fd, &ok, 1) != 1)
        return 1;
    close(ready_fd);
    for (;;) {
        uint32_t rbuf[128];
        size_t got = 0;
        if (write_read(&c, NULL, 0, rbuf, sizeof(rbuf), &got) < 0)
            return 1;
        char* p = (char*)rbuf;
        char* end = p + got;
        while (p < end) {
            uint32_t cmd = *(uint32_t*)p;
            p += sizeof(uint32_t);
            if (cmd == BR_NOOP || cmd == BR_TRANSACTION_COMPLETE || cmd == BR_SPAWN_LOOPER)
                continue;
            if (cmd != BR_TRANSACTION && cmd != BR_TRANSACTION_SEC_CTX) {
                printf("bindertest: server: unexpected command %#x\n", cmd);
                return 1;
            }
            struct binder_transaction_data* tr;
            if (cmd == BR_TRANSACTION_SEC_CTX) {
                struct binder_transaction_data_secctx* sc = (struct binder_transaction_data_secctx*)p;
                tr = &sc->transaction_data;
                if (!sc->secctx || !*(const char*)(uintptr_t)sc->secctx) {
                    printf("bindertest: FAIL: no security context with the call\n");
                    return 1;
                }
                p += sizeof(struct binder_transaction_data_secctx);
            } else {
                tr = (struct binder_transaction_data*)p;
                p += sizeof(*tr);
            }
            const void* buffer = (const void*)(uintptr_t)tr->data.ptr.buffer;
            if (tr->code == CODE_QUIT) {
                uint32_t answer = 0;
                server_reply(&c, buffer, &answer, sizeof(answer), 0);
                return 0;
            }
            if (tr->code == CODE_PING) {
                uint32_t v = *(const uint32_t*)buffer;
                uint32_t answer = v * 2 + 1;
                server_reply(&c, buffer, &answer, sizeof(answer), 0);
            } else if (tr->code == CODE_FD) {
                // data: one flat_binder_object (BINDER_TYPE_FD); offsets: its position
                const struct binder_fd_object* obj = buffer;
                uint32_t sum = 0;
                if (tr->offsets_size == sizeof(binder_size_t) && obj->hdr.type == BINDER_TYPE_FD) {
                    char text[64] = {0};
                    lseek(obj->fd, 0, SEEK_SET);
                    ssize_t n = read(obj->fd, text, sizeof(text) - 1);
                    for (ssize_t i = 0; i < n; i++)
                        sum += (unsigned char)text[i];
                    close(obj->fd);
                }
                server_reply(&c, buffer, &sum, sizeof(sum), 0);
            }
        }
    }
}

// ---------------------------------------------------------------- client

static int transact(struct conn* c, uint32_t code, const void* data, size_t len, const binder_size_t* offsets,
                    size_t offsets_len, uint32_t* answer) {
    struct {
        uint32_t cmd;
        struct binder_transaction_data tr;
    } __attribute__((packed)) w = {0};
    w.cmd = BC_TRANSACTION;
    w.tr.target.handle = 0;
    w.tr.code = code;
    w.tr.flags = TF_ACCEPT_FDS;
    w.tr.data_size = len;
    w.tr.offsets_size = offsets_len;
    w.tr.data.ptr.buffer = (uintptr_t)data;
    w.tr.data.ptr.offsets = (uintptr_t)offsets;
    const void* wptr = &w;
    size_t wlen = sizeof(w);
    for (;;) {
        uint32_t rbuf[128];
        size_t got = 0;
        if (write_read(c, wptr, wlen, rbuf, sizeof(rbuf), &got) < 0)
            return -1;
        wptr = NULL;
        wlen = 0;
        char* p = (char*)rbuf;
        char* end = p + got;
        while (p < end) {
            uint32_t cmd = *(uint32_t*)p;
            p += sizeof(uint32_t);
            if (cmd == BR_NOOP || cmd == BR_TRANSACTION_COMPLETE)
                continue;
            if (cmd == BR_REPLY) {
                struct binder_transaction_data* tr = (struct binder_transaction_data*)p;
                p += sizeof(*tr);
                const void* buffer = (const void*)(uintptr_t)tr->data.ptr.buffer;
                if (answer && tr->data_size >= sizeof(uint32_t))
                    *answer = *(const uint32_t*)buffer;
                struct {
                    uint32_t cmd;
                    binder_uintptr_t ptr;
                } __attribute__((packed)) f = {BC_FREE_BUFFER, (uintptr_t)buffer};
                write_read(c, &f, sizeof(f), NULL, 0, NULL);
                return 0;
            }
            printf("bindertest: client: unexpected command %#x\n", cmd);
            return -1;
        }
    }
}

static int client(const char* dev) {
    struct conn c;
    if (conn_open(&c, dev) < 0)
        return 1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int good = 0;
    for (uint32_t i = 0; i < ROUNDS; i++) {
        uint32_t answer = 0;
        if (transact(&c, CODE_PING, &i, sizeof(i), NULL, 0, &answer) < 0)
            break;
        if (answer == i * 2 + 1)
            good++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
    CHECK(good == ROUNDS, "%s: %d of %d round trips answered correctly", dev, good, ROUNDS);
    if (good == ROUNDS)
        printf("bindertest: %s: %d round trips, %ld us each\n", dev, ROUNDS, us / ROUNDS);

    // A descriptor through binder: a memfd with known text.
    int mfd = memfd_create("bindertest", MFD_CLOEXEC);
    const char text[] = "hello through binder";
    uint32_t expect = 0;
    for (size_t i = 0; i < sizeof(text) - 1; i++)
        expect += (unsigned char)text[i];
    CHECK(mfd >= 0 && write(mfd, text, sizeof(text) - 1) == (ssize_t)sizeof(text) - 1, "memfd");
    struct binder_fd_object obj = {.hdr.type = BINDER_TYPE_FD, .fd = (uint32_t)mfd};
    binder_size_t off = 0;
    uint32_t sum = 0;
    int r = transact(&c, CODE_FD, &obj, sizeof(obj), &off, sizeof(off), &sum);
    CHECK(r == 0 && sum == expect, "%s: descriptor passing (checksum %u, expected %u)", dev, sum, expect);
    if (r == 0 && sum == expect)
        printf("bindertest: %s: a memfd passed to the other process and read there\n", dev);
    uint32_t dummy;
    transact(&c, CODE_QUIT, &dummy, sizeof(dummy), NULL, 0, NULL);
    return failures != 0;
}

static void pingpong(const char* dev) {
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return;
    pid_t srv = fork();
    if (srv == 0) {
        close(pipefd[0]);
        _exit(server(dev, pipefd[1]));
    }
    close(pipefd[1]);
    char ok = 0;
    if (read(pipefd[0], &ok, 1) != 1) {
        CHECK(0, "%s: the server didn't start", dev);
        waitpid(srv, NULL, 0);
        return;
    }
    close(pipefd[0]);
    pid_t cli = fork();
    if (cli == 0)
        _exit(client(dev));
    int st = 0;
    waitpid(cli, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "%s: client exit status %#x", dev, st);
    waitpid(srv, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "%s: server exit status %#x", dev, st);
}

static void binderfs_test(void) {
    const char* dir = "/tmp/binderfs";
    mkdir(dir, 0755);
    if (mount("binder", dir, "binder", 0, NULL) < 0) {
        CHECK(0, "mount -t binder: %s", strerror(errno));
        return;
    }
    struct stat st;
    CHECK(stat("/tmp/binderfs/binder", &st) == 0 && S_ISCHR(st.st_mode), "binderfs has no binder device");
    CHECK(stat("/tmp/binderfs/hwbinder", &st) == 0, "binderfs has no hwbinder");
    CHECK(stat("/tmp/binderfs/vndbinder", &st) == 0, "binderfs has no vndbinder");
    CHECK(access("/tmp/binderfs/features/oneway_spam_detection", R_OK) == 0, "binderfs has no features/");
    int ctl = open("/tmp/binderfs/binder-control", O_RDONLY | O_CLOEXEC);
    CHECK(ctl >= 0, "open binder-control: %s", strerror(errno));
    if (ctl >= 0) {
        struct binderfs_device d = {0};
        strcpy(d.name, "anbox-binder");
        int r = ioctl(ctl, BINDER_CTL_ADD, &d);
        CHECK(r == 0, "BINDER_CTL_ADD: %s", strerror(errno));
        if (r == 0)
            printf("bindertest: binderfs: added anbox-binder (%u:%u)\n", d.major, d.minor);
        r = ioctl(ctl, BINDER_CTL_ADD, &d);
        CHECK(r < 0 && errno == EEXIST, "BINDER_CTL_ADD of an existing name: %d %s", r, strerror(errno));
        close(ctl);
    }
    pingpong("/tmp/binderfs/anbox-binder");
    pingpong("/tmp/binderfs/binder"); // a context of its own: its manager is free again
    umount2(dir, MNT_DETACH);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) {
        pingpong(argv[1]);
    } else {
        // /dev/binder: open + protocol version only. Becoming its context
        // manager here would tie it to root for the rest of the boot (binder
        // remembers the first manager's uid); the ping-pong runs on fresh
        // binderfs devices instead.
        struct conn c;
        CHECK(conn_open(&c, "/dev/binder") == 0, "/dev/binder");
        close(c.fd);
        binderfs_test();
    }
    printf("bindertest: %s\n", failures ? "FAILED" : "all binder tests passed");
    return failures != 0;
}
