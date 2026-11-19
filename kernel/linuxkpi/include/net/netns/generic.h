#pragma once
#include <net/net_namespace.h>
/* Per-namespace data of pernet_operations with .id/.size (one namespace). */
void* net_generic(const struct net* net, unsigned int id);
