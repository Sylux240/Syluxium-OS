// Loopback socket tests: TCP bulk transfer + half-close, connection
// refused, non-blocking connect with poll, UDP datagrams.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (cond) printf("  [ ok ] %s\n", what);                                  \
        else { printf("  [FAIL] %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } \
    } while (0)

static struct sockaddr_in addr(int port) {
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

#define BULK (1024 * 1024)

int main(void) {
    printf("nettest: loopback sockets\n");

    // ---- TCP bulk transfer ----
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = addr(7001);
    CHECK(bind(ls, (struct sockaddr*)&a, sizeof(a)) == 0, "bind 127.0.0.1:7001");
    CHECK(listen(ls, 4) == 0, "listen");

    pid_t child = fork();
    if (child == 0) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in s = addr(7001);
        if (connect(c, (struct sockaddr*)&s, sizeof(s)) != 0) _exit(10);
        unsigned char* buf = malloc(BULK);
        for (int i = 0; i < BULK; i++) buf[i] = (unsigned char)(i * 7 + (i >> 9));
        size_t sent = 0;
        while (sent < BULK) {
            ssize_t n = write(c, buf + sent, BULK - sent);
            if (n <= 0) _exit(11);
            sent += n;
        }
        shutdown(c, SHUT_WR); // half-close: we still read the reply
        char reply[64] = {0};
        ssize_t n = read(c, reply, sizeof(reply) - 1);
        close(c);
        _exit(n > 0 && strcmp(reply, "got it") == 0 ? 0 : 12);
    }
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int s = accept(ls, (struct sockaddr*)&peer, &plen);
    CHECK(s >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "accept from 127.0.0.1");
    unsigned char* got = malloc(BULK);
    size_t total = 0;
    for (;;) {
        ssize_t n = read(s, got + total, BULK - total < 65536 ? BULK - total : 65536);
        if (n <= 0) break;
        total += n;
        if (total == BULK) {
            char extra;
            n = read(s, &extra, 1); // must be EOF (peer shut down writing)
            if (n != 0) total = 0;
            break;
        }
    }
    int ok = total == BULK;
    for (int i = 0; ok && i < BULK; i++) ok = got[i] == (unsigned char)(i * 7 + (i >> 9));
    CHECK(ok, "1 MiB transferred intact, then EOF");
    CHECK(write(s, "got it", 6) == 6, "reply after peer's half-close");
    close(s);
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "client saw the reply");
    close(ls);

    // ---- connection refused ----
    int r = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in closed = addr(7999);
    CHECK(connect(r, (struct sockaddr*)&closed, sizeof(closed)) == -1 && errno == ECONNREFUSED,
          "connect to a closed port: ECONNREFUSED");
    close(r);

    // ---- non-blocking connect ----
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    a = addr(7002);
    bind(ls, (struct sockaddr*)&a, sizeof(a));
    listen(ls, 1);
    int nb = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int rc = connect(nb, (struct sockaddr*)&a, sizeof(a));
    CHECK(rc == 0 || errno == EINPROGRESS, "non-blocking connect returns EINPROGRESS");
    struct pollfd p = {nb, POLLOUT, 0};
    int err = -1;
    socklen_t elen = sizeof(err);
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT) &&
          getsockopt(nb, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0,
          "poll says writable, SO_ERROR is 0");
    int acc = accept(ls, NULL, NULL);
    CHECK(acc >= 0, "accept the non-blocking client");
    close(acc);
    close(nb);
    close(ls);

    // ---- UDP ----
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = addr(7003);
    CHECK(bind(u1, (struct sockaddr*)&ua, sizeof(ua)) == 0, "UDP bind");
    CHECK(sendto(u2, "datagram!", 9, 0, (struct sockaddr*)&ua, sizeof(ua)) == 9, "UDP sendto");
    char ubuf[32] = {0};
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t un = recvfrom(u1, ubuf, sizeof(ubuf), 0, (struct sockaddr*)&from, &flen);
    CHECK(un == 9 && memcmp(ubuf, "datagram!", 9) == 0 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "UDP recvfrom with sender address");
    close(u1);
    close(u2);

    printf("nettest: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
