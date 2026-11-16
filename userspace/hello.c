// The classic. Built with the host's gcc and glibc: a genuine Linux binary.
#include <stdio.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(void) {
    struct utsname u;
    uname(&u);
    printf("Hello from user space! I'm pid %d, running on %s %s (%s).\n", getpid(), u.sysname, u.release,
           u.machine);
    return 0;
}
