// Netlink (NETLINK_ROUTE) tests: glibc's getifaddrs() and if_nameindex()
// (both use RTM_GETLINK/RTM_GETADDR dumps), a raw RTM_GETROUTE dump, an
// acknowledged request, policy routing rules (RTM_NEWRULE/DELRULE/GETRULE,
// as Android's netd uses them).
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/fib_rules.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, what)                                                                       \
    do {                                                                                        \
        if (cond) printf("  [ ok ] %s\n", what);                                                \
        else { printf("  [FAIL] %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } \
    } while (0)

static void test_getifaddrs(void) {
    struct ifaddrs* list;
    CHECK(getifaddrs(&list) == 0, "getifaddrs()");
    int lo4 = 0, lo6 = 0, packet = 0;
    for (struct ifaddrs* i = list; i; i = i->ifa_next) {
        char buf[INET6_ADDRSTRLEN] = "-";
        int fam = i->ifa_addr ? i->ifa_addr->sa_family : -1;
        if (fam == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in*)i->ifa_addr)->sin_addr, buf, sizeof(buf));
        else if (fam == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)i->ifa_addr)->sin6_addr, buf, sizeof(buf));
        printf("         %-6s family %2d flags %#06x %s\n", i->ifa_name, fam, i->ifa_flags, buf);
        if (fam == AF_INET && strcmp(i->ifa_name, "lo") == 0 && strcmp(buf, "127.0.0.1") == 0) lo4 = 1;
        if (fam == AF_INET6 && strcmp(i->ifa_name, "lo") == 0 && strcmp(buf, "::1") == 0) lo6 = 1;
        if (fam == AF_PACKET) packet++;
    }
    freeifaddrs(list);
    CHECK(lo4, "lo has 127.0.0.1");
    CHECK(packet >= 1, "AF_PACKET (link) entries present");
    if (lo6) printf("  [ ok ] lo has ::1\n");

    struct if_nameindex* ni = if_nameindex();
    int n = 0;
    for (struct if_nameindex* p = ni; p && p->if_index; p++) n++;
    CHECK(ni && n >= 1 && if_nametoindex("lo") >= 1, "if_nameindex() / if_nametoindex(\"lo\")");
    if (ni) if_freenameindex(ni);
}

static void test_route_dump(void) {
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    CHECK(s >= 0, "socket(AF_NETLINK, NETLINK_ROUTE)");
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    CHECK(bind(s, (struct sockaddr*)&sa, sizeof(sa)) == 0, "bind netlink");
    socklen_t sl = sizeof(sa);
    CHECK(getsockname(s, (struct sockaddr*)&sa, &sl) == 0 && sa.nl_pid != 0, "kernel assigned a port id");

    struct {
        struct nlmsghdr h;
        struct rtmsg r;
    } req = {{sizeof(req), RTM_GETROUTE, NLM_F_REQUEST | NLM_F_DUMP, 42, 0}, {.rtm_family = AF_INET}};
    CHECK(send(s, &req, sizeof(req), 0) == sizeof(req), "send RTM_GETROUTE dump");
    char buf[8192];
    int routes = 0, done = 0, defaults = 0;
    while (!done) {
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (struct nlmsghdr* h = (struct nlmsghdr*)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_seq != 42) continue;
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (h->nlmsg_type == RTM_NEWROUTE) {
                struct rtmsg* r = NLMSG_DATA(h);
                routes++;
                if (r->rtm_dst_len == 0) defaults++;
            }
        }
    }
    printf("         %d IPv4 route(s), %d default\n", routes, defaults);
    CHECK(done, "route dump ends with NLMSG_DONE");

    // An unsupported request with NLM_F_ACK gets an NLMSG_ERROR carrying a code.
    struct nlmsghdr bad = {sizeof(bad), RTM_DELLINK, NLM_F_REQUEST | NLM_F_ACK, 43, 0};
    send(s, &bad, sizeof(bad), 0);
    ssize_t n = recv(s, buf, sizeof(buf), 0);
    struct nlmsghdr* h = (struct nlmsghdr*)buf;
    CHECK(n > 0 && h->nlmsg_type == NLMSG_ERROR && ((struct nlmsgerr*)NLMSG_DATA(h))->error < 0,
          "NLMSG_ERROR for an unsupported request");
    close(s);
}


// Rules: count the ones with priority `prio` (and fwmark `mark` if non-zero) in a dump.
static int count_rules(int s, int family, unsigned prio, unsigned mark, int* total) {
    struct {
        struct nlmsghdr h;
        struct fib_rule_hdr r;
    } req = {{sizeof(req), RTM_GETRULE, NLM_F_REQUEST | NLM_F_DUMP, 60, 0}, {.family = family}};
    send(s, &req, sizeof(req), 0);
    char buf[8192];
    int found = 0, done = 0;
    *total = 0;
    while (!done) {
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (struct nlmsghdr* h = (struct nlmsghdr*)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (h->nlmsg_type != RTM_NEWRULE) continue;
            (*total)++;
            unsigned p = 0, m = 0;
            int len = h->nlmsg_len - NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
            for (struct rtattr* a = (struct rtattr*)((char*)NLMSG_DATA(h) + sizeof(struct fib_rule_hdr));
                 RTA_OK(a, len); a = RTA_NEXT(a, len)) {
                if (a->rta_type == FRA_PRIORITY) memcpy(&p, RTA_DATA(a), 4);
                if (a->rta_type == FRA_FWMARK) memcpy(&m, RTA_DATA(a), 4);
            }
            if (p == prio && (!mark || m == mark)) found++;
        }
    }
    return found;
}

static int rule_request(int s, int type, int flags, unsigned prio, unsigned mark, unsigned table) {
    struct {
        struct nlmsghdr h;
        struct fib_rule_hdr r;
        struct rtattr a1; unsigned v1;
        struct rtattr a2; unsigned v2;
        struct rtattr a3; unsigned v3;
    } req;
    memset(&req, 0, sizeof(req));
    req.h = (struct nlmsghdr){sizeof(req), type, NLM_F_REQUEST | NLM_F_ACK | flags, 61, 0};
    req.r.family = AF_INET;
    req.r.action = FR_ACT_TO_TBL;
    req.a1 = (struct rtattr){8, FRA_PRIORITY}; req.v1 = prio;
    req.a2 = (struct rtattr){8, FRA_FWMARK}; req.v2 = mark;
    req.a3 = (struct rtattr){8, FRA_TABLE}; req.v3 = table;
    send(s, &req, sizeof(req), 0);
    char buf[512];
    ssize_t n = recv(s, buf, sizeof(buf), 0);
    struct nlmsghdr* h = (struct nlmsghdr*)buf;
    if (n <= 0 || h->nlmsg_type != NLMSG_ERROR) return -1000;
    return ((struct nlmsgerr*)NLMSG_DATA(h))->error;
}

static void test_rules(void) {
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    bind(s, (struct sockaddr*)&sa, sizeof(sa));
    int total;
    CHECK(count_rules(s, AF_INET, 32766, 0, &total) == 1, "default rule: main table at priority 32766");
    CHECK(rule_request(s, RTM_NEWRULE, NLM_F_CREATE, 13000, 0x10063, 1003) == 0, "RTM_NEWRULE (fwmark, table 1003)");
    CHECK(count_rules(s, AF_INET, 13000, 0x10063, &total) == 1, "the new rule is in the dump");
    CHECK(rule_request(s, RTM_NEWRULE, NLM_F_CREATE | NLM_F_EXCL, 13000, 0x10063, 1003) == -EEXIST,
          "the same rule again with NLM_F_EXCL: EEXIST");
    CHECK(rule_request(s, RTM_DELRULE, 0, 13000, 0x10063, 1003) == 0, "RTM_DELRULE");
    CHECK(count_rules(s, AF_INET, 13000, 0, &total) == 0, "the rule is gone");
    CHECK(rule_request(s, RTM_DELRULE, 0, 13000, 0x10063, 1003) == -ENOENT, "deleting it again: ENOENT");
    close(s);
}

int main(void) {
    printf("nltest: netlink\n");
    test_getifaddrs();
    test_route_dump();
    test_rules();
    printf("nltest: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
