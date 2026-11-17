// Pseudo-terminals: what xterm/Konsole/ssh do.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c, what) do { if (c) printf("  [ ok ] %s\n", what); else { printf("  [FAIL] %s (errno %d %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

// Read from the master until `want` shows up (or 3 s pass).
static int read_until(int m, char* buf, size_t size, const char* want) {
    size_t len = 0;
    buf[0] = 0;
    for (int tries = 0; tries < 60 && !strstr(buf, want); tries++) {
        struct pollfd p = {m, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0) continue;
        ssize_t n = read(m, buf + len, size - 1 - len);
        if (n <= 0) break;
        len += n;
        buf[len] = 0;
    }
    return strstr(buf, want) != NULL;
}

int main(void) {
    printf("ptytest: pseudo-terminals\n");
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0, "posix_openpt + grantpt + unlockpt");
    char* name = ptsname(m);
    CHECK(name && strncmp(name, "/dev/pts/", 9) == 0, "ptsname gives /dev/pts/N");
    struct winsize ws = {.ws_row = 30, .ws_col = 100};
    CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ on the master");

    pid_t child = fork();
    if (child == 0) {
        setsid();
        int s = open(name, O_RDWR);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        close(m);
        execl("/bin/sh", "sh", "-c", "stty size; read line; echo \"got:$line\"; exec sleep 100", (char*)NULL);
        _exit(127);
    }
    char buf[4096];
    CHECK(read_until(m, buf, sizeof(buf), "30 100"), "the shell on the slave sees the window size (stty size)");
    write(m, "hello pty\r", 10);
    CHECK(read_until(m, buf, sizeof(buf), "got:hello pty"), "typed line reaches the program (CR -> NL, canonical mode)");
    CHECK(strstr(buf, "hello pty") && strstr(buf, "\r\n"), "input is echoed and output NL becomes CR NL");
    write(m, "\x03", 1); // Ctrl+C
    int status = 0;
    pid_t r = 0;
    for (int i = 0; i < 40 && r == 0; i++) { usleep(50000); r = waitpid(child, &status, WNOHANG); }
    CHECK(r == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT, "Ctrl+C on the master -> SIGINT for the foreground job");
    // The slave is closed now (child gone): the master reads EOF as EIO.
    while (read(m, buf, sizeof(buf)) > 0) ;
    CHECK(read(m, buf, sizeof(buf)) == -1 && errno == EIO, "master read after the slave closed -> EIO (terminal emulators exit)");
    close(m);

    // raw mode + forkpty (what most programs use)
    int master;
    pid_t c2 = forkpty(&master, NULL, NULL, NULL);
    if (c2 == 0) {
        struct termios t;
        tcgetattr(0, &t);
        cfmakeraw(&t);
        tcsetattr(0, TCSANOW, &t);
        char ch;
        read(0, &ch, 1); // raw: one byte, no Enter needed, no echo
        printf("[%c]", ch);
        fflush(stdout);
        _exit(0);
    }
    usleep(100000);
    write(master, "Z", 1);
    CHECK(read_until(master, buf, sizeof(buf), "[Z]") && !strstr(buf, "Z[") , "forkpty + raw mode: single key, no echo");
    waitpid(c2, NULL, 0);
    close(master);

    printf("ptytest: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
