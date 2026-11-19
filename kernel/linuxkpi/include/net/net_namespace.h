/* LinuxKPI: network namespaces. Syluxium has exactly one (init_net). */
#ifndef _LKPI_NET_NET_NAMESPACE_H
#define _LKPI_NET_NET_NAMESPACE_H

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/atomic.h>

LKPI_EXTERN_C_BEGIN

struct user_namespace;
struct sock;
struct net_device;
struct ns_common { unsigned int inum; refcount_t count; };

struct net {
    refcount_t passive;
    struct list_head list;
    struct user_namespace* user_ns;
    struct ns_common ns;
    struct sock* genl_sock;
    struct sock* rtnl;
    int ifindex;
};
extern struct net init_net;

typedef struct { struct net* net; } possible_net_t;
static inline void write_pnet(possible_net_t* pnet, struct net* net) { pnet->net = net; }
static inline struct net* read_pnet(const possible_net_t* pnet) { return pnet->net ? pnet->net : &init_net; }
static inline int net_eq(const struct net* a, const struct net* b) { return a == b; }
static inline struct net* get_net(struct net* net) { return net; }
static inline void put_net(struct net* net) {}
static inline struct net* maybe_get_net(struct net* net) { return net; }
static inline int check_net(const struct net* net) { return 1; }
#define for_each_net(VAR) for (VAR = &init_net; VAR; VAR = NULL)
#define for_each_net_rcu(VAR) for_each_net(VAR)
#define net_ns_get(net) (net)
static inline struct net* get_net_ns_by_pid(int pid) { return &init_net; }
static inline struct net* get_net_ns_by_fd(int fd) { return &init_net; }

struct pernet_operations {
    struct list_head list;
    int (*init)(struct net* net);
    void (*pre_exit)(struct net* net);
    void (*exit)(struct net* net);
    void (*exit_batch)(struct list_head* net_exit_list);
    unsigned int* id;
    size_t size;
};
int register_pernet_device(struct pernet_operations* ops);
void unregister_pernet_device(struct pernet_operations* ops);
#define register_pernet_subsys register_pernet_device
#define unregister_pernet_subsys unregister_pernet_device

LKPI_EXTERN_C_END

#endif
