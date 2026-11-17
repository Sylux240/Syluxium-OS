// AF_UNIX socket tests: socketpair, path and abstract names, stream echo
// across fork, datagrams, seqpacket boundaries, SCM_RIGHTS fd passing,
// SCM_CREDENTIALS / SO_PEERCRED, poll readiness, EOF/EPIPE, non-blocking.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, what)                                                                       \
    do {                                                                                        \
        if (cond) printf("  [ ok ] %s\n", what);                                                \
        else { printf("  [FAIL] %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } \
    } while (0)

static socklen_t path_addr(struct sockaddr_un* a, const char* path) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, path);
    return offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
}

static socklen_t abstract_addr(struct sockaddr_un* a, const char* name) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path + 1, name, strlen(name));
    return offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);
}

// Send one byte plus `nfds` descriptors.
static int send_fds(int sock, const int* fds, int nfds) {
    char byte = 'F';
    struct iovec iov = {&byte, 1};
    char ctl[CMSG_SPACE(sizeof(int) * 8)];
    memset(ctl, 0, sizeof(ctl));
    struct msghdr m = {0};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
    struct cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    return sendmsg(sock, &m, 0) == 1 ? 0 : -1;
}

// Receive one byte plus up to `max` descriptors; returns how many arrived.
static int recv_fds(int sock, int* fds, int max) {
    char byte;
    struct iovec iov = {&byte, 1};
    char ctl[CMSG_SPACE(sizeof(int) * 8)];
    struct msghdr m = {0};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = sizeof(ctl);
    if (recvmsg(sock, &m, MSG_CMSG_CLOEXEC) != 1)
        return -1;
    int n = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            if (n > max) n = max;
            memcpy(fds, CMSG_DATA(c), sizeof(int) * n);
        }
    return n;
}

static void test_socketpair(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair(SOCK_STREAM)");
    char buf[64];
    CHECK(write(sv[0], "hello", 5) == 5 && read(sv[1], buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0,
          "socketpair: data flows 0 -> 1");
    CHECK(write(sv[1], "world!", 6) == 6 && read(sv[0], buf, sizeof(buf)) == 6 && memcmp(buf, "world!", 6) == 0,
          "socketpair: data flows 1 -> 0");
    struct ucred cr;
    socklen_t len = sizeof(cr);
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 && cr.pid == getpid(),
          "SO_PEERCRED names our pid");
    int domain = 0;
    len = sizeof(domain);
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0 && domain == AF_UNIX, "SO_DOMAIN = AF_UNIX");

    // EOF and EPIPE once the other end is gone; poll reports it.
    write(sv[1], "x", 1);
    close(sv[1]);
    struct pollfd p = {sv[0], POLLIN | POLLOUT, 0};
    CHECK(poll(&p, 1, 1000) >= 1 && (p.revents & POLLIN) && (p.revents & POLLHUP), "poll: POLLIN|POLLHUP after close");
    CHECK(read(sv[0], buf, sizeof(buf)) == 1, "queued data still readable after peer close");
    CHECK(read(sv[0], buf, sizeof(buf)) == 0, "then EOF");
    errno = 0;
    CHECK(send(sv[0], "y", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "send to closed peer: EPIPE");
    close(sv[0]);

    // Non-blocking: EAGAIN on empty, then fill the queue until EAGAIN.
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0, "socketpair(SOCK_NONBLOCK)");
    errno = 0;
    CHECK(read(sv[0], buf, sizeof(buf)) < 0 && errno == EAGAIN, "empty non-blocking read: EAGAIN");
    static char big[65536];
    long total = 0;
    for (;;) {
        ssize_t n = write(sv[0], big, sizeof(big));
        if (n < 0) break;
        total += n;
        if (total > 64 * 1024 * 1024) break;
    }
    CHECK(errno == EAGAIN && total > 0 && total < 64 * 1024 * 1024, "full queue: write gives EAGAIN");
    p.fd = sv[0];
    p.events = POLLOUT;
    CHECK(poll(&p, 1, 0) == 0, "full queue: no POLLOUT");
    long drained = 0;
    for (;;) {
        ssize_t n = read(sv[1], big, sizeof(big));
        if (n <= 0) break;
        drained += n;
    }
    CHECK(drained == total, "everything written is read back");
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT), "drained: POLLOUT again");
    close(sv[0]);
    close(sv[1]);
}

static void test_path_stream(void) {
    const char* path = "/tmp/unixtest.sock";
    unlink(path);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    socklen_t alen = path_addr(&a, path);
    CHECK(bind(ls, (struct sockaddr*)&a, alen) == 0, "bind /tmp/unixtest.sock");
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISSOCK(st.st_mode), "the path is a socket file");
    errno = 0;
    int dup_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(dup_sock, (struct sockaddr*)&a, alen) < 0 && errno == EADDRINUSE, "second bind: EADDRINUSE");
    close(dup_sock);
    CHECK(listen(ls, 8) == 0, "listen");
    struct sockaddr_un got;
    socklen_t glen = sizeof(got);
    CHECK(getsockname(ls, (struct sockaddr*)&got, &glen) == 0 && glen == alen && strcmp(got.sun_path, path) == 0,
          "getsockname returns the path");

    pid_t child = fork();
    if (child == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr*)&a, alen) != 0) _exit(10);
        struct sockaddr_un peer;
        socklen_t plen = sizeof(peer);
        if (getpeername(c, (struct sockaddr*)&peer, &plen) != 0 || strcmp(peer.sun_path, path) != 0) _exit(11);
        // 300 KiB in odd-sized writes: more than one queue's worth.
        static unsigned char data[300 * 1024];
        for (unsigned i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i * 13 + (i >> 8));
        size_t sent = 0;
        while (sent < sizeof(data)) {
            size_t n = sizeof(data) - sent < 7777 ? sizeof(data) - sent : 7777;
            ssize_t w = write(c, data + sent, n);
            if (w <= 0) _exit(12);
            sent += w;
        }
        shutdown(c, SHUT_WR);
        char reply[16];
        ssize_t r = read(c, reply, sizeof(reply));
        if (r != 2 || memcmp(reply, "OK", 2) != 0) _exit(13);
        _exit(0);
    }
    struct sockaddr_un from;
    socklen_t flen = sizeof(from);
    int s = accept(ls, (struct sockaddr*)&from, &flen);
    CHECK(s >= 0, "accept");
    CHECK(flen == sizeof(sa_family_t), "accepted peer is unnamed");
    struct ucred cr;
    socklen_t crlen = sizeof(cr);
    CHECK(getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) == 0 && cr.pid == child, "SO_PEERCRED = client pid");
    static unsigned char in[300 * 1024 + 16];
    size_t got_bytes = 0;
    for (;;) {
        ssize_t r = read(s, in + got_bytes, sizeof(in) - got_bytes);
        if (r <= 0) break;
        got_bytes += r;
    }
    int ok = got_bytes == 300 * 1024;
    for (unsigned i = 0; ok && i < 300 * 1024; i++)
        if (in[i] != (unsigned char)(i * 13 + (i >> 8))) ok = 0;
    CHECK(ok, "300 KiB stream intact, then EOF on SHUT_WR");
    CHECK(write(s, "OK", 2) == 2, "reply after half-close");
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "client saw the server's address and reply");
    close(s);

    FILE* f = fopen("/proc/net/unix", "r");
    char line[256];
    int listed = 0;
    while (f && fgets(line, sizeof(line), f))
        if (strstr(line, path)) listed = 1;
    if (f) fclose(f);
    CHECK(listed, "/proc/net/unix lists the listener");

    close(ls);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    CHECK(connect(c, (struct sockaddr*)&a, alen) < 0 && errno == ECONNREFUSED, "listener closed: ECONNREFUSED");
    CHECK(unlink(path) == 0, "unlink the socket file");
    errno = 0;
    CHECK(connect(c, (struct sockaddr*)&a, alen) < 0 && errno == ENOENT, "after unlink: ENOENT");
    close(c);
}

static void test_abstract(void) {
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    socklen_t alen = abstract_addr(&a, "syluxium-test");
    CHECK(bind(ls, (struct sockaddr*)&a, alen) == 0 && listen(ls, 1) == 0, "bind+listen abstract @syluxium-test");
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(connect(c, (struct sockaddr*)&a, alen) == 0, "connect to abstract name (non-blocking connects at once)");
    struct pollfd p = {ls, POLLIN, 0};
    CHECK(poll(&p, 1, 1000) == 1, "listener polls readable");
    int s = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    char buf[8];
    CHECK(s >= 0 && send(c, "ping", 4, 0) == 4 && recv(s, buf, sizeof(buf), 0) == 4, "abstract stream echo");
    struct sockaddr_un got;
    socklen_t glen = sizeof(got);
    CHECK(getsockname(s, (struct sockaddr*)&got, &glen) == 0 && glen == alen && got.sun_path[0] == 0 &&
              memcmp(got.sun_path + 1, "syluxium-test", 13) == 0,
          "accepted socket's name is the abstract listener name");
    close(s);
    close(c);
    close(ls);

    // Autobind: bind with just the family.
    int x = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un fam = {.sun_family = AF_UNIX};
    glen = sizeof(got);
    CHECK(bind(x, (struct sockaddr*)&fam, sizeof(sa_family_t)) == 0 &&
              getsockname(x, (struct sockaddr*)&got, &glen) == 0 && glen == 8 && got.sun_path[0] == 0,
          "autobind gives a 5-digit abstract name");
    close(x);
}

static void test_dgram(void) {
    unlink("/tmp/ud-a");
    unlink("/tmp/ud-b");
    int a = socket(AF_UNIX, SOCK_DGRAM, 0), b = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un aa, ba, from;
    socklen_t al = path_addr(&aa, "/tmp/ud-a"), bl = path_addr(&ba, "/tmp/ud-b");
    CHECK(bind(a, (struct sockaddr*)&aa, al) == 0 && bind(b, (struct sockaddr*)&ba, bl) == 0, "bind two datagram sockets");
    CHECK(sendto(a, "one", 3, 0, (struct sockaddr*)&ba, bl) == 3 && sendto(a, "two!", 4, 0, (struct sockaddr*)&ba, bl) == 4,
          "sendto twice");
    char buf[16];
    socklen_t fl = sizeof(from);
    ssize_t n1 = recvfrom(b, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
    CHECK(n1 == 3 && memcmp(buf, "one", 3) == 0 && strcmp(from.sun_path, "/tmp/ud-a") == 0,
          "datagram boundaries kept, sender address reported");
    CHECK(recv(b, buf, 2, 0) == 2, "short read of a datagram");
    errno = 0;
    CHECK(recv(b, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN, "rest of truncated datagram discarded");
    CHECK(connect(a, (struct sockaddr*)&ba, bl) == 0 && send(a, "conn", 4, 0) == 4 && recv(b, buf, sizeof(buf), 0) == 4,
          "connected datagram socket");
    int u = socket(AF_UNIX, SOCK_DGRAM, 0);
    fl = sizeof(from);
    CHECK(sendto(u, "anon", 4, 0, (struct sockaddr*)&ba, bl) == 4 &&
              recvfrom(b, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl) == 4 && fl == 0,
          "unnamed sender: empty address");
    close(u);
    close(b);
    errno = 0;
    CHECK(send(a, "gone", 4, 0) < 0 && errno == ECONNREFUSED, "peer closed: ECONNREFUSED");
    close(a);
    unlink("/tmp/ud-a");
    unlink("/tmp/ud-b");

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair(SOCK_SEQPACKET)");
    send(sv[0], "abc", 3, 0);
    send(sv[0], "defgh", 5, 0);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) == 3 && recv(sv[1], buf, sizeof(buf), 0) == 5, "seqpacket keeps records");
    send(sv[0], "0123456789", 10, 0);
    CHECK(recv(sv[1], buf, 4, MSG_TRUNC) == 10, "MSG_TRUNC returns the full record length");
    close(sv[0]);
    CHECK(recv(sv[1], buf, sizeof(buf), 0) == 0, "seqpacket EOF");
    close(sv[1]);
}

static void test_fd_passing(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int pipefd[2];
    pipe(pipefd);
    const char* fpath = "/tmp/unixtest.txt";
    int f = open(fpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(f, "file contents", 13);
    lseek(f, 5, SEEK_SET); // the offset is shared with the receiver

    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        int fds[4];
        int n = recv_fds(sv[1], fds, 4);
        if (n != 2) _exit(20);
        if (!(fcntl(fds[0], F_GETFD) & FD_CLOEXEC)) _exit(21); // MSG_CMSG_CLOEXEC
        if (write(fds[0], "via pipe", 8) != 8) _exit(22);
        char buf[16] = {0};
        if (read(fds[1], buf, sizeof(buf)) != 8 || memcmp(buf, "contents", 8) != 0) _exit(23);
        _exit(0);
    }
    close(sv[1]);
    int pass[2] = {pipefd[1], f};
    CHECK(send_fds(sv[0], pass, 2) == 0, "sendmsg with SCM_RIGHTS (pipe + file)");
    close(pipefd[1]);
    close(f);
    char buf[16] = {0};
    CHECK(read(pipefd[0], buf, sizeof(buf)) == 8 && memcmp(buf, "via pipe", 8) == 0,
          "child wrote through the passed pipe");
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child read the passed file at the shared offset");
    CHECK(read(pipefd[0], buf, sizeof(buf)) == 0, "pipe EOF: every passed copy closed");
    close(pipefd[0]);
    close(sv[0]);
    unlink(fpath);

    // Data plus fds sent but read with plain read(): the fds are dropped, data survives.
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int p2[2];
    pipe(p2);
    send_fds(sv[0], &p2[1], 1);
    close(p2[1]);
    char c;
    CHECK(read(sv[1], &c, 1) == 1 && c == 'F', "read() of a message carrying fds");
    CHECK(read(p2[0], buf, 1) == 0, "the discarded fd was closed (pipe EOF)");
    close(p2[0]);
    close(sv[0]);
    close(sv[1]);
}

static void test_credentials(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    int one = 1;
    CHECK(setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) == 0, "SO_PASSCRED");
    pid_t child = fork();
    if (child == 0) {
        send(sv[0], "c", 1, 0);
        _exit(0);
    }
    waitpid(child, NULL, 0);
    char byte;
    struct iovec iov = {&byte, 1};
    char ctl[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr m = {0};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl;
    m.msg_controllen = sizeof(ctl);
    int found = 0;
    if (recvmsg(sv[1], &m, 0) == 1)
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_type == SCM_CREDENTIALS) {
                struct ucred cr;
                memcpy(&cr, CMSG_DATA(c), sizeof(cr));
                found = cr.pid == child && cr.uid == 0;
            }
    CHECK(found, "SCM_CREDENTIALS carries the sender's pid");
    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    printf("unixtest: AF_UNIX sockets\n");
    signal(SIGPIPE, SIG_IGN);
    test_socketpair();
    test_path_stream();
    test_abstract();
    test_dgram();
    test_fd_passing();
    test_credentials();
    printf("unixtest: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
