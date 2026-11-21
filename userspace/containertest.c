// Container primitives, the way LXC (Waydroid's Android container) uses them:
//   1. unshare(CLONE_NEWUTS) + sethostname: a private host name
//   2. a root assembled from bind mounts (directories and a single file)
//   3. pivot_root into it, mount proc, sysfs, cgroup2 there, detach the old root
//   4. a dynamic program (uname) runs inside and sees the container's names
//   5. chroot: ".." can't leave the new root
//   6. setns(/proc/<pid>/ns/mnt): another process enters the container's root
// Needs root. Leaves nothing mounted behind.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("containertest: FAIL: " __VA_ARGS__); printf(" (%s)\n", strerror(errno)); failures++; } } while (0)

#define ROOT "/tmp/ctr"

static void mkdirs(void) {
    const char* dirs[] = {ROOT, ROOT "/usr", ROOT "/etc", ROOT "/proc", ROOT "/sys", ROOT "/dev", ROOT "/oldroot",
                          ROOT "/cgroup", ROOT "/tmp"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        mkdir(dirs[i], 0755);
    symlink("usr/bin", ROOT "/bin");
    symlink("usr/lib", ROOT "/lib");
    symlink("usr/lib64", ROOT "/lib64");
    int fd = open(ROOT "/etc/os-release", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, "placeholder\n", 12) != 12) { }
        close(fd);
    }
    fd = open("/tmp/ctr-hostfile", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, "from the host\n", 14) != 14) { }
        close(fd);
    }
}

static int read_file(const char* path, char* buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n > 0 ? (int)n : -1;
}

// The container's first process (pid 1 in a real one).
static int container(int ready_fd, int go_fd) {
    char buf[256];
    struct utsname u;
    CHECK(unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC) == 0, "unshare");
    CHECK(sethostname("android", 7) == 0, "sethostname");
    CHECK(uname(&u) == 0 && !strcmp(u.nodename, "android"), "uname after sethostname: %s", u.nodename);

    CHECK(mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0, "make / private");
    CHECK(mount("/usr", ROOT "/usr", NULL, MS_BIND | MS_REC, NULL) == 0, "bind /usr");
    CHECK(mount("/tmp/ctr-hostfile", ROOT "/etc/os-release", NULL, MS_BIND, NULL) == 0, "bind a file");
    CHECK(read_file(ROOT "/etc/os-release", buf, sizeof(buf)) > 0 && !strcmp(buf, "from the host\n"),
          "file bind mount shows %s", buf);
    CHECK(mount(ROOT "/etc/os-release", ROOT "/etc/os-release", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0,
          "remount read-only");
    // Recursive bind: a tmpfs mounted below the source shows below the target.
    mkdir(ROOT "/tmp/src", 0755);
    mkdir(ROOT "/tmp/src/sub", 0755);
    mkdir(ROOT "/tmp/dst", 0755);
    CHECK(mount("tmpfs", ROOT "/tmp/src/sub", "tmpfs", 0, NULL) == 0, "tmpfs for the recursive bind");
    int mk = open(ROOT "/tmp/src/sub/marker", O_CREAT | O_WRONLY, 0644);
    if (mk >= 0)
        close(mk);
    CHECK(mount(ROOT "/tmp/src", ROOT "/tmp/dst", NULL, MS_BIND | MS_REC, NULL) == 0, "recursive bind");
    CHECK(access(ROOT "/tmp/dst/sub/marker", F_OK) == 0, "the submount came along with MS_REC");

    CHECK(chdir(ROOT) == 0, "chdir");
    CHECK(syscall(SYS_pivot_root, ".", "oldroot") == 0, "pivot_root");
    CHECK(chdir("/") == 0, "chdir /");
    struct stat st;
    CHECK(stat("/oldroot/etc/hostname", &st) == 0 || stat("/oldroot/etc", &st) == 0, "old root reachable before detach");
    CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0, "mount proc");
    CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0, "mount sysfs");
    CHECK(mount("cgroup2", "/cgroup", "cgroup2", 0, NULL) == 0, "mount cgroup2");
    CHECK(read_file("/proc/self/comm", buf, sizeof(buf)) > 0, "/proc/self/comm inside");
    CHECK(mkdir("/cgroup/android", 0755) == 0, "cgroup mkdir");
    CHECK(access("/cgroup/android/cgroup.procs", W_OK) == 0, "cgroup.procs in a new group");
    int fd = open("/cgroup/android/cgroup.procs", O_WRONLY);
    CHECK(fd >= 0 && write(fd, "1\n", 2) == 2, "write cgroup.procs");
    if (fd >= 0)
        close(fd);
    CHECK(rmdir("/cgroup/android") == 0, "cgroup rmdir");
    CHECK(umount2("/oldroot", MNT_DETACH) == 0, "detach the old root");
    CHECK(stat("/oldroot/etc", &st) < 0, "old root gone after detach");
    CHECK(stat("/../../usr/bin", &st) == 0 && stat("/../oldroot/etc", &st) < 0, "\"..\" stays inside");

    // Tell the parent we're set up, wait for its setns() check.
    char c = 1;
    if (write(ready_fd, &c, 1) != 1 || read(go_fd, &c, 1) != 1) { }

    // A dynamic program from the bind-mounted /usr, inside.
    pid_t pid = fork();
    if (pid == 0) {
        int out = open("/tmp/uname.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        dup2(out, 1);
        execl("/bin/uname", "uname", "-n", (char*)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "uname inside (status %#x)", status);
    CHECK(read_file("/tmp/uname.txt", buf, sizeof(buf)) > 0 && !strcmp(buf, "android\n"), "uname -n inside says %s", buf);
    unlink("/tmp/uname.txt");

    umount2("/cgroup", MNT_DETACH);
    umount2("/sys", MNT_DETACH);
    umount2("/proc", MNT_DETACH);
    umount2("/etc/os-release", MNT_DETACH);
    umount2("/usr", MNT_DETACH);
    return failures;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (geteuid() != 0) {
        printf("containertest: needs root\n");
        return 1;
    }
    char before[65];
    gethostname(before, sizeof(before));
    mkdirs();

    int ready[2], go[2];
    if (pipe(ready) < 0 || pipe(go) < 0)
        return 1;
    pid_t ctr = fork();
    if (ctr == 0)
        _exit(container(ready[1], go[0]));
    char c;
    if (read(ready[0], &c, 1) != 1) {
        printf("containertest: FAIL: the container didn't come up\n");
        failures++;
    } else {
        // setns from outside: a helper enters the container's mount namespace.
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/ns/mnt", ctr);
        pid_t helper = fork();
        if (helper == 0) {
            int fd = open(path, O_RDONLY);
            if (fd < 0 || setns(fd, CLONE_NEWNS) < 0)
                _exit(2);
            struct stat st;
            // Inside: the container's / has /cgroup but no /oldroot/etc.
            _exit(stat("/cgroup", &st) == 0 && stat("/oldroot/etc", &st) < 0 ? 0 : 3);
        }
        int st = 0;
        waitpid(helper, &st, 0);
        errno = 0;
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "setns into the container's root (status %#x)", st);
        // Mount namespace: the container's mounts aren't the host's.
        struct stat sb;
        CHECK(stat(ROOT "/usr/bin", &sb) < 0, "the container's bind mount of /usr is visible on the host");
        CHECK(stat(ROOT "/tmp/dst/sub/marker", &sb) < 0, "the container's recursive bind is visible on the host");
        if (write(go[1], &c, 1) != 1) { }
    }
    int status = 0;
    waitpid(ctr, &status, 0);
    errno = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "container exit status %#x", status);

    // chroot: ".." stops at the new root.
    pid_t ch = fork();
    if (ch == 0) {
        struct stat st;
        if (chroot(ROOT) < 0 || chdir("/") < 0)
            _exit(2);
        _exit(stat("/../../usr", &st) == 0 && stat("/../../tmp/ctr", &st) < 0 ? 0 : 3);
    }
    waitpid(ch, &status, 0);
    errno = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "chroot confinement (status %#x)", status);

    char after[65];
    gethostname(after, sizeof(after));
    errno = 0;
    CHECK(!strcmp(before, after), "the system's host name changed: %s -> %s", before, after);
    printf("containertest: %s\n", failures ? "FAILED" : "all container tests passed");
    return failures != 0;
}
