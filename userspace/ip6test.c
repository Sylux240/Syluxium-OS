// IPv6 + IP fragmentation tests: TCP and UDP over ::1, dual-stack AF_INET6
// sockets with v4-mapped peers, IPV6_V6ONLY, ICMPv6 echo on ::1, the
// link-local address, and UDP datagrams bigger than the MTU (fragmented
// and reassembled) over both IPv4 and IPv6.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, what)                                                                       \
    do {                                                                                        \
        if (cond) printf("  [ ok ] %s\n", what);                                                \
        else { printf("  [FAIL] %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } \
    } while (0)

static struct sockaddr_in6 addr6(const char* ip, int port) {
    struct sockaddr_in6 a = {0};
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return a;
}

#define BULK (512 * 1024)

static void test_tcp6(void) {
    int ls = socket(AF_INET6, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in6 a = addr6("::1", 7101);
    CHECK(ls >= 0 && bind(ls, (struct sockaddr*)&a, sizeof(a)) == 0 && listen(ls, 4) == 0, "TCP listen on [::1]:7101");
    pid_t child = fork();
    if (child == 0) {
        int c = socket(AF_INET6, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr*)&a, sizeof(a)) != 0) _exit(10);
        unsigned char* buf = malloc(BULK);
        for (int i = 0; i < BULK; i++) buf[i] = (unsigned char)(i * 11 + (i >> 10));
        size_t sent = 0;
        while (sent < BULK) {
            ssize_t w = write(c, buf + sent, BULK - sent);
            if (w <= 0) _exit(11);
            sent += w;
        }
        close(c);
        _exit(0);
    }
    struct sockaddr_in6 peer;
    socklen_t plen = sizeof(peer);
    int s = accept(ls, (struct sockaddr*)&peer, &plen);
    char text[64] = "";
    if (s >= 0) inet_ntop(AF_INET6, &peer.sin6_addr, text, sizeof(text));
    CHECK(s >= 0 && plen == sizeof(peer) && strcmp(text, "::1") == 0, "accept from ::1");
    unsigned char* in = malloc(BULK + 16);
    size_t got = 0;
    for (;;) {
        ssize_t r = read(s, in + got, BULK + 16 - got);
        if (r <= 0) break;
        got += r;
    }
    int ok = got == BULK;
    for (int i = 0; ok && i < BULK; i++)
        if (in[i] != (unsigned char)(i * 11 + (i >> 10))) ok = 0;
    CHECK(ok, "512 KiB over IPv6 loopback intact");
    int status;
    waitpid(child, &status, 0);
    close(s);
    close(ls);
    free(in);
}

static void test_dual_stack(void) {
    // An AF_INET6 listener on :: also takes IPv4 clients (as ::ffff:127.0.0.1).
    int ls = socket(AF_INET6, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in6 any = addr6("::", 7102);
    CHECK(bind(ls, (struct sockaddr*)&any, sizeof(any)) == 0 && listen(ls, 4) == 0, "TCP listen on [::]:7102");
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in v4 = {.sin_family = AF_INET, .sin_port = htons(7102)};
    v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(c, (struct sockaddr*)&v4, sizeof(v4)) == 0, "IPv4 client connects to the IPv6 wildcard listener");
    struct sockaddr_in6 peer;
    socklen_t plen = sizeof(peer);
    int s = accept(ls, (struct sockaddr*)&peer, &plen);
    char text[64] = "";
    inet_ntop(AF_INET6, &peer.sin6_addr, text, sizeof(text));
    CHECK(s >= 0 && strcmp(text, "::ffff:127.0.0.1") == 0, "peer reported as ::ffff:127.0.0.1");
    char buf[8];
    CHECK(write(c, "v4", 2) == 2 && read(s, buf, sizeof(buf)) == 2, "data over the v4-mapped connection");
    close(s);
    close(c);
    close(ls);

    // IPV6_V6ONLY: the same listener refuses IPv4.
    ls = socket(AF_INET6, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    CHECK(setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) == 0, "IPV6_V6ONLY");
    any.sin6_port = htons(7103);
    bind(ls, (struct sockaddr*)&any, sizeof(any));
    listen(ls, 4);
    c = socket(AF_INET, SOCK_STREAM, 0);
    v4.sin_port = htons(7103);
    errno = 0;
    CHECK(connect(c, (struct sockaddr*)&v4, sizeof(v4)) < 0 && errno == ECONNREFUSED, "v6only listener: IPv4 refused");
    close(c);
    close(ls);
}

static void test_udp(int family, const char* what, size_t size) {
    int r = socket(family, SOCK_DGRAM, 0), w = socket(family, SOCK_DGRAM, 0);
    struct sockaddr_storage ss = {0};
    socklen_t len;
    if (family == AF_INET6) {
        struct sockaddr_in6 a = addr6("::1", 7104);
        memcpy(&ss, &a, sizeof(a));
        len = sizeof(a);
    } else {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(7104)};
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        memcpy(&ss, &a, sizeof(a));
        len = sizeof(a);
    }
    bind(r, (struct sockaddr*)&ss, len);
    unsigned char* out = malloc(size);
    unsigned char* in = malloc(size + 1);
    for (size_t i = 0; i < size; i++) out[i] = (unsigned char)(i * 7 + 3);
    ssize_t sent = sendto(w, out, size, 0, (struct sockaddr*)&ss, len);
    struct pollfd p = {r, POLLIN, 0};
    ssize_t got = poll(&p, 1, 2000) == 1 ? recv(r, in, size + 1, 0) : -1;
    char msg[96];
    snprintf(msg, sizeof(msg), "%s UDP datagram of %zu bytes", what, size);
    CHECK(sent == (ssize_t)size && got == (ssize_t)size && memcmp(in, out, size) == 0, msg);
    free(out);
    free(in);
    close(r);
    close(w);
}

static void test_ping6(void) {
    int s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
    CHECK(s >= 0, "ICMPv6 ping socket");
    struct sockaddr_in6 dst = addr6("::1", 0);
    struct icmp6_hdr req = {0};
    req.icmp6_type = ICMP6_ECHO_REQUEST;
    req.icmp6_seq = htons(1);
    char pkt[64];
    memcpy(pkt, &req, sizeof(req));
    memcpy(pkt + sizeof(req), "syluxium", 8);
    sendto(s, pkt, sizeof(req) + 8, 0, (struct sockaddr*)&dst, sizeof(dst));
    struct pollfd p = {s, POLLIN, 0};
    char buf[128];
    ssize_t n = poll(&p, 1, 2000) == 1 ? recv(s, buf, sizeof(buf), 0) : -1;
    CHECK(n == (ssize_t)sizeof(req) + 8 && (unsigned char)buf[0] == ICMP6_ECHO_REPLY && memcmp(buf + 8, "syluxium", 8) == 0,
          "echo reply from ::1");
    close(s);
}

static void test_link_local(void) {
    FILE* f = fopen("/proc/net/if_inet6", "r");
    char line[128];
    int ll = 0, lo = 0;
    while (f && fgets(line, sizeof(line), f)) {
        printf("         %s", line);
        if (strncmp(line, "fe80", 4) == 0) ll = 1;
        if (strncmp(line, "00000000000000000000000000000001", 32) == 0) lo = 1;
    }
    if (f) fclose(f);
    CHECK(lo, "::1 on lo");
    CHECK(ll, "a link-local fe80:: address on the network card");
}

int main(void) {
    printf("ip6test: IPv6 and fragmentation\n");
    test_link_local();
    test_tcp6();
    test_dual_stack();
    test_ping6();
    test_udp(AF_INET6, "IPv6", 1000);
    test_udp(AF_INET, "IPv4 fragmented", 20000);
    test_udp(AF_INET6, "IPv6 fragmented", 30000);
    printf("ip6test: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
