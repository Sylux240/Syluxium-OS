// Unprivileged user namespaces, the way bubblewrap and Chromium's sandbox
// use them. Runs as root, does its work as uid/gid 1000:
//   1. without a user namespace: unshare(CLONE_NEWNS) and mount are refused
//   2. unshare(CLONE_NEWUSER | CLONE_NEWNS): setgroups "deny", uid_map/gid_map
//      "0 1000 1"; then uid 0 inside, every capability (capget), a private
//      tmpfs the rest of the system doesn't see, sethostname refused (the UTS
//      namespace isn't ours), someone else's uid can't be mapped
//   3. clone(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS): pid 1 in the child,
//      proc mounted there, chroot; capabilities dropped with capset are gone
//   4. exec in the namespace as its root keeps uid 0 (id -u says 0)
//   5. PR_SET_NO_NEW_PRIVS sticks
// Leaves nothing behind.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR "/tmp/usernstest"
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("usernstest: FAIL: " __VA_ARGS__); printf(" (%s)\n", strerror(errno)); failures++; } } while (0)

static int write_file(const char* path, const char* text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    int e = errno;
    close(fd);
    errno = e;
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int has_caps(void) {
    struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct d[2];
    memset(d, 0, sizeof(d));
    if (syscall(SYS_capget, &h, d) != 0)
        return -1;
    return d[0].effective != 0;
}

static int map_self(const char* pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/setgroups", pid);
    if (write_file(path, "deny") != 0)
        return -1;
    snprintf(path, sizeof(path), "/proc/%s/uid_map", pid);
    if (write_file(path, "0 1000 1") != 0)
        return -2;
    snprintf(path, sizeof(path), "/proc/%s/gid_map", pid);
    if (write_file(path, "0 1000 1") != 0)
        return -3;
    return 0;
}

static int as_user(void) {
    return setgroups(0, NULL) == 0 && setgid(1000) == 0 && setuid(1000) == 0 ? 0 : -1;
}

static int child_pidns(void* arg) {
    (void)arg;
    // Wait for the parent to write our maps.
    char c;
    int* fds = arg;
    close(fds[1]);
    if (read(fds[0], &c, 1) != 1)
        return 10;
    if (getpid() != 1)
        return 11;
    if (getuid() != 0)
        return 12;
    if (mount("proc", DIR, "proc", 0, NULL) != 0)
        return 13;
    if (access(DIR "/self/stat", R_OK) != 0)
        return 14;
    umount2(DIR, MNT_DETACH);
    if (chroot("/proc/self/fdinfo") != 0)
        return 15;
    // Drop every capability, as Chromium's sandbox does, and see them gone.
    struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct d[2];
    memset(d, 0, sizeof(d));
    if (syscall(SYS_capset, &h, d) != 0)
        return 16;
    if (has_caps() != 0)
        return 17;
    if (chroot("/") == 0)
        return 18; // no capabilities, no chroot
    return 0;
}

int main(void) {
    mkdir(DIR, 0755);
    chown(DIR, 1000, 1000);

    // 1 + 2 in a child that becomes uid 1000.
    pid_t pid = fork();
    if (pid == 0) {
        if (as_user() != 0)
            _exit(20);
        if (unshare(CLONE_NEWNS) == 0 || errno != EPERM)
            _exit(21);
        if (mount("tmpfs", DIR, "tmpfs", 0, NULL) == 0)
            _exit(22);
        if (has_caps() != 0)
            _exit(23);
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
            _exit(24);
        if (has_caps() != 1)
            _exit(25);
        if (getuid() != 65534)
            _exit(26); // not mapped yet: the overflow id
        if (write_file("/proc/self/uid_map", "0 0 1") == 0)
            _exit(27); // someone else's (root's) id
        int r = map_self("self");
        if (r)
            _exit(30 - r);
        if (getuid() != 0 || geteuid() != 0 || getgid() != 0)
            _exit(33);
        char buf[64] = {};
        int fd = open("/proc/self/uid_map", O_RDONLY);
        if (fd < 0 || read(fd, buf, sizeof(buf) - 1) <= 0 || !strstr(buf, "1000"))
            _exit(34);
        close(fd);
        if (mount("tmpfs", DIR, "tmpfs", 0, NULL) != 0)
            _exit(35);
        if (write_file(DIR "/inside", "x") != 0 && (fd = open(DIR "/inside", O_CREAT | O_WRONLY, 0644)) < 0)
            _exit(36);
        struct stat st;
        if (stat(DIR "/inside", &st) != 0 || st.st_uid != 0)
            _exit(37); // our own file: owner 0 as the namespace names it
        if (sethostname("x", 1) == 0)
            _exit(38);
        if (unshare(CLONE_NEWUTS) != 0 || sethostname("usernstest", 10) != 0)
            _exit(39);
        // 4: exec as the namespace's root
        int p[2];
        if (pipe(p) != 0)
            _exit(40);
        pid_t c = fork();
        if (c == 0) {
            dup2(p[1], 1);
            execl("/usr/bin/id", "id", "-u", (char*)NULL);
            _exit(127);
        }
        close(p[1]);
        memset(buf, 0, sizeof(buf));
        if (read(p[0], buf, sizeof(buf) - 1) <= 0 || strcmp(buf, "0\n") != 0)
            _exit(41);
        waitpid(c, NULL, 0);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "unshare(CLONE_NEWUSER) as a user: step %d", WEXITSTATUS(status));
    CHECK(access(DIR "/inside", F_OK) != 0, "the namespace's tmpfs stayed private");
    char host[65] = {};
    gethostname(host, sizeof(host));
    CHECK(strcmp(host, "usernstest") != 0, "the host name stayed the system's");

    // 3: clone(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS) as a user.
    pid = fork();
    if (pid == 0) {
        if (as_user() != 0)
            _exit(50);
        int fds[2];
        if (pipe(fds) != 0)
            _exit(51);
        static char stack[256 * 1024];
        pid_t c = clone(child_pidns, stack + sizeof(stack), CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD, fds);
        if (c < 0)
            _exit(52);
        close(fds[0]);
        char path[32];
        snprintf(path, sizeof(path), "%d", c);
        // The parent (in the parent namespace, as the owner) writes the maps.
        char p2[64];
        snprintf(p2, sizeof(p2), "/proc/%d/setgroups", c);
        if (write_file(p2, "deny") != 0)
            _exit(53);
        snprintf(p2, sizeof(p2), "/proc/%d/uid_map", c);
        if (write_file(p2, "0 1000 1") != 0)
            _exit(54);
        snprintf(p2, sizeof(p2), "/proc/%d/gid_map", c);
        if (write_file(p2, "0 1000 1") != 0)
            _exit(55);
        if (write(fds[1], "g", 1) != 1)
            _exit(56);
        int st;
        waitpid(c, &st, 0);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 99);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "clone(NEWUSER|NEWPID|NEWNS) as a user: step %d", WEXITSTATUS(status));

    // 5: no new privileges
    pid = fork();
    if (pid == 0) {
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
            _exit(1);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "PR_SET_NO_NEW_PRIVS");

    rmdir(DIR);
    if (!failures)
        printf("usernstest: all user namespace checks passed\n");
    return failures ? 1 : 0;
}
