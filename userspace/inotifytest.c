// inotify: what file managers and desktop menus rely on.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static int failures = 0;
#define CHECK(c, what) do { if (c) printf("  [ ok ] %s\n", what); else { printf("  [FAIL] %s (errno %d %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

static char buf[8192];
static int nev;
static struct { int wd; unsigned mask, cookie; char name[64]; } evs[64];

static void drain(int fd) {
    nev = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            struct inotify_event* e = (struct inotify_event*)p;
            if (nev < 64) {
                evs[nev].wd = e->wd; evs[nev].mask = e->mask; evs[nev].cookie = e->cookie;
                snprintf(evs[nev].name, sizeof(evs[nev].name), "%s", e->len ? e->name : "");
                nev++;
            }
            p += sizeof(*e) + e->len;
        }
    }
}

static int has(unsigned mask, const char* name) {
    for (int i = 0; i < nev; i++)
        if ((evs[i].mask & mask) == mask && strcmp(evs[i].name, name) == 0) return i + 1;
    return 0;
}

int main(void) {
    printf("inotifytest: file change notifications\n");
    mkdir("/tmp/watched", 0755);
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int wd = inotify_add_watch(fd, "/tmp/watched", IN_ALL_EVENTS);
    CHECK(fd >= 0 && wd > 0, "inotify_init1 + add_watch on a directory");
    CHECK(read(fd, buf, sizeof(buf)) == -1 && errno == EAGAIN, "nothing yet -> EAGAIN");

    int f = open("/tmp/watched/a.txt", O_CREAT | O_WRONLY, 0644);
    write(f, "hello", 5);
    close(f);
    int avail = 0;
    ioctl(fd, FIONREAD, &avail);
    struct pollfd p = {fd, POLLIN, 0};
    CHECK(avail > 0 && poll(&p, 1, 0) == 1, "FIONREAD > 0 and poll says readable");
    drain(fd);
    CHECK(has(IN_CREATE, "a.txt") && has(IN_MODIFY, "a.txt") && has(IN_CLOSE_WRITE, "a.txt"),
          "create + write + close -> IN_CREATE, IN_MODIFY, IN_CLOSE_WRITE (with the name)");

    f = open("/tmp/watched/a.txt", O_RDONLY);
    close(f);
    drain(fd);
    CHECK(has(IN_OPEN, "a.txt") && has(IN_CLOSE_NOWRITE, "a.txt"), "open read-only -> IN_OPEN, IN_CLOSE_NOWRITE");

    rename("/tmp/watched/a.txt", "/tmp/watched/b.txt");
    drain(fd);
    int from = has(IN_MOVED_FROM, "a.txt"), to = has(IN_MOVED_TO, "b.txt");
    CHECK(from && to && evs[from - 1].cookie == evs[to - 1].cookie && evs[from - 1].cookie != 0,
          "rename -> IN_MOVED_FROM/IN_MOVED_TO with the same cookie");

    utime("/tmp/watched/b.txt", NULL);
    mkdir("/tmp/watched/sub", 0755);
    rmdir("/tmp/watched/sub");
    drain(fd);
    CHECK(has(IN_CREATE | IN_ISDIR, "sub") && has(IN_DELETE | IN_ISDIR, "sub"), "mkdir/rmdir -> IN_CREATE/IN_DELETE | IN_ISDIR");

    // a watch on the file itself
    int fwd = inotify_add_watch(fd, "/tmp/watched/b.txt", IN_ATTRIB | IN_DELETE_SELF | IN_ONESHOT);
    truncate("/tmp/watched/b.txt", 0);
    utime("/tmp/watched/b.txt", NULL);
    drain(fd);
    int attrib = 0, ignored = 0;
    for (int i = 0; i < nev; i++) {
        if (evs[i].wd == fwd && (evs[i].mask & IN_ATTRIB)) attrib++;
        if (evs[i].wd == fwd && (evs[i].mask & IN_IGNORED)) ignored++;
    }
    CHECK(attrib == 1 && ignored == 1, "IN_ONESHOT file watch: one IN_ATTRIB, then IN_IGNORED");

    fwd = inotify_add_watch(fd, "/tmp/watched/b.txt", IN_DELETE_SELF);
    unlink("/tmp/watched/b.txt");
    drain(fd);
    int del_self = 0;
    for (int i = 0; i < nev; i++)
        if (evs[i].wd == fwd && (evs[i].mask & IN_DELETE_SELF)) del_self = 1;
    CHECK(has(IN_DELETE, "b.txt") && del_self, "unlink -> IN_DELETE on the directory, IN_DELETE_SELF on the file");

    CHECK(inotify_rm_watch(fd, wd) == 0, "inotify_rm_watch");
    drain(fd);
    CHECK(nev == 1 && (evs[0].mask & IN_IGNORED), "removing the watch queues IN_IGNORED");
    f = open("/tmp/watched/c.txt", O_CREAT | O_WRONLY, 0644);
    close(f);
    drain(fd);
    CHECK(nev == 0, "no events after the watch is gone");
    CHECK(inotify_add_watch(fd, "/nonexistent", IN_CREATE) == -1 && errno == ENOENT, "add_watch on a missing path -> ENOENT");

    printf("inotifytest: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
