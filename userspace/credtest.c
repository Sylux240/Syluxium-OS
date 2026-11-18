// Users and permissions: what a multi-user system must enforce.
// Runs as root, prepares files, then a child becomes uid/gid 1000 and checks
// that it can do exactly what an ordinary user may. "credtest --report" is
// run setuid-root by that child and prints its ids.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c, what) do { if (c) printf("  [ ok ] %s\n", what); else { printf("  [FAIL] %s (errno %d %s)\n", what, errno, strerror(errno)); failures++; } } while (0)
#define DENIED(call, err) ((call) < 0 && errno == (err))

static const char* SUID_COPY = "/tmp/credtest-suid";

static int copy_file(const char* from, const char* to, mode_t mode) {
    int in = open(from, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (in < 0 || out < 0) return -1;
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n) return -1;
    close(in);
    close(out);
    return chmod(to, mode);
}

static void as_user(void) {
    gid_t groups[] = {1000, 29 /* audio */};
    if (setgroups(2, groups) || setgid(1000) || setuid(1000)) {
        printf("  [FAIL] could not become uid 1000\n");
        exit(1);
    }
    CHECK(getuid() == 1000 && geteuid() == 1000 && getgid() == 1000, "ids are now 1000/1000");
    CHECK(DENIED(setuid(0), EPERM), "can't setuid(0) back to root");
    CHECK(DENIED(setgroups(1, groups), EPERM), "can't change supplementary groups");
    gid_t got[8];
    CHECK(getgroups(8, got) == 2 && got[1] == 29, "getgroups returns the groups set by root");
    CHECK(setresuid(-1, 1000, -1) == 0, "setresuid to its own id is allowed");

    int fd = open("/etc/passwd", O_RDONLY);
    CHECK(fd >= 0, "reads /etc/passwd");
    if (fd >= 0) close(fd);
    CHECK(DENIED(open("/etc/passwd", O_WRONLY), EACCES), "can't write /etc/passwd");
    CHECK(DENIED(open("/etc/credtest-new", O_WRONLY | O_CREAT, 0644), EACCES), "can't create files in /etc");
    CHECK(DENIED(mkdir("/etc/credtest-dir", 0755), EACCES), "can't mkdir in /etc");
    CHECK(access("/etc/passwd", R_OK) == 0 && DENIED(access("/etc/passwd", W_OK), EACCES), "access(): R_OK yes, W_OK no");

    fd = open("/tmp/credtest-user", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    struct stat st;
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == 1000 && st.st_gid == 1000, "new files belong to the user");
    if (fd >= 0) close(fd);
    CHECK(chown("/tmp/credtest-user", -1, 29) == 0, "chown own file to one of its groups");
    CHECK(DENIED(chown("/tmp/credtest-user", -1, 0), EPERM), "can't chown own file to group root");
    CHECK(DENIED(chown("/tmp/credtest-user", 0, -1), EPERM), "can't give a file away");
    CHECK(unlink("/tmp/credtest-user") == 0, "deletes its own file in /tmp");

    CHECK(DENIED(open("/tmp/credtest-root", O_WRONLY), EACCES), "can't write root's 0644 file");
    CHECK(DENIED(unlink("/tmp/credtest-root"), EPERM), "sticky /tmp: can't delete root's file");
    CHECK(DENIED(rename("/tmp/credtest-root", "/tmp/credtest-stolen"), EPERM), "sticky /tmp: can't rename it");
    CHECK(DENIED(chmod("/tmp/credtest-root", 0666), EPERM), "can't chmod root's file");
    CHECK(DENIED(open("/tmp/credtest-priv/secret", O_RDONLY), EACCES), "can't reach into a 0700 root directory");
    CHECK(DENIED(chdir("/tmp/credtest-priv"), EACCES), "can't chdir into it either");

    fd = open("/dev/null", O_RDWR);
    CHECK(fd >= 0, "/dev/null is usable");
    if (fd >= 0) close(fd);
    fd = open("/proc/self/status", O_RDONLY);
    CHECK(fd >= 0, "reads /proc/self/status");
    if (fd >= 0) close(fd);
    CHECK(DENIED(kill(getppid(), 0), EPERM), "can't signal root's processes");

    // A setuid-root program runs as root (effective), with AT_SECURE.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execl(SUID_COPY, SUID_COPY, "--report", (char*)NULL);
        _exit(99);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "setuid-root program: euid 0, uid 1000, AT_SECURE");
    exit(failures ? 1 : 0);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--report") == 0) {
        int ok = geteuid() == 0 && getuid() == 1000 && getauxval(AT_SECURE) == 1;
        if (!ok)
            printf("  (report: uid %d euid %d secure %lu)\n", getuid(), geteuid(), getauxval(AT_SECURE));
        return ok ? 0 : 1;
    }
    printf("credtest: users and permissions\n");
    // Root's setup.
    unlink("/tmp/credtest-root");
    int fd = open("/tmp/credtest-root", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "root creates /tmp/credtest-root (0644)");
    if (fd >= 0) close(fd);
    mkdir("/tmp/credtest-priv", 0700);
    chmod("/tmp/credtest-priv", 0700);
    fd = open("/tmp/credtest-priv/secret", O_WRONLY | O_CREAT, 0600);
    if (fd >= 0) close(fd);
    struct stat st;
    CHECK(stat("/tmp", &st) == 0 && (st.st_mode & 01777) == 01777, "/tmp is 1777 (sticky)");
    CHECK(copy_file(argv[0], SUID_COPY, 04755) == 0 && stat(SUID_COPY, &st) == 0 && (st.st_mode & 04000) && st.st_uid == 0,
          "a setuid-root copy of this program");
    fd = open("/tmp/credtest-root", O_RDONLY);
    CHECK(fd >= 0 && fchmod(fd, 0) == 0, "root may chmod 0 ...");
    if (fd >= 0) close(fd);
    fd = open("/tmp/credtest-root", O_RDWR);
    CHECK(fd >= 0, "... and still open a mode-0 file (root)");
    if (fd >= 0) close(fd);
    chmod("/tmp/credtest-root", 0644);
    CHECK(DENIED(execl("/tmp/credtest-root", "x", (char*)NULL), EACCES), "even root can't exec a file without x bits");

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
        as_user();
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        failures++;
    CHECK(access("/tmp/credtest-root", F_OK) == 0, "root's file survived");
    unlink("/tmp/credtest-root");
    unlink("/tmp/credtest-priv/secret");
    rmdir("/tmp/credtest-priv");
    unlink(SUID_COPY);
    printf("credtest: %s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
