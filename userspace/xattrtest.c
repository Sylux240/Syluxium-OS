// Extended attributes: set/get/list/remove with Linux's flags and errnos
// (what Android's installd does on every app data directory).
// Prints PASS/FAIL per check; exit status = number of failures.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

int main(void) {
    const char* f = "/tmp/xattr-test";
    const char* d = "/tmp/xattr-dir";
    int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0644);
    mkdir(d, 0755);
    char buf[64];
    check(getxattr(f, "user.none", buf, sizeof(buf)) < 0 && errno == ENODATA, "missing attribute: ENODATA");
    check(setxattr(f, "user.a", "hello", 5, 0) == 0, "setxattr user.a");
    check(getxattr(f, "user.a", NULL, 0) == 5, "size query returns the length");
    memset(buf, 0, sizeof(buf));
    check(getxattr(f, "user.a", buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0, "getxattr returns the value");
    check(getxattr(f, "user.a", buf, 2) < 0 && errno == ERANGE, "small buffer: ERANGE");
    check(setxattr(f, "user.a", "x", 1, XATTR_CREATE) < 0 && errno == EEXIST, "XATTR_CREATE on an existing one: EEXIST");
    check(setxattr(f, "user.b", "x", 1, XATTR_REPLACE) < 0 && errno == ENODATA, "XATTR_REPLACE on a missing one: ENODATA");
    check(fsetxattr(fd, "user.b", "world", 5, 0) == 0, "fsetxattr user.b");
    check(setxattr(d, "user.default", "", 0, 0) == 0 && getxattr(d, "user.default", buf, sizeof(buf)) == 0,
          "an empty value on a directory");
    ssize_t n = listxattr(f, buf, sizeof(buf));
    int has_a = 0, has_b = 0;
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(buf + i) + 1) {
        has_a |= strcmp(buf + i, "user.a") == 0;
        has_b |= strcmp(buf + i, "user.b") == 0;
    }
    check(n == 14 && has_a && has_b, "listxattr names both");
    check(removexattr(f, "user.a") == 0 && getxattr(f, "user.a", buf, sizeof(buf)) < 0, "removexattr");
    check(removexattr(f, "user.a") < 0 && errno == ENODATA, "removing again: ENODATA");
    check(setxattr(f, "bogus.a", "x", 1, 0) < 0 && errno == EOPNOTSUPP, "unknown namespace: EOPNOTSUPP");
    close(fd);
    unlink(f);
    rmdir(d);
    printf("xattrtest: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
