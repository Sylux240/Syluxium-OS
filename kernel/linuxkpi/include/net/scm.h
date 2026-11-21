/* LinuxKPI: socket control messages (credentials are not passed on yet). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/lkpi_binder.h>
#include <linux/socket.h>

struct scm_creds {
    u32 pid;
    kuid_t uid;
    kgid_t gid;
};
struct scm_cookie {
    struct pid *pid;
    struct scm_creds creds;
};
struct socket;
static inline void scm_recv(struct socket *sock, struct msghdr *msg, struct scm_cookie *scm, int flags) {}
static inline void scm_destroy(struct scm_cookie *scm) {}
static inline int scm_send(struct socket *sock, struct msghdr *msg, struct scm_cookie *scm, bool forcecreds)
{
    __builtin_memset(scm, 0, sizeof(*scm));
    return 0;
}
