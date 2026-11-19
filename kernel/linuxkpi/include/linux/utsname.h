#pragma once
#include <linux/kernel.h>
struct new_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
struct new_utsname* init_utsname(void);
#define utsname() init_utsname()
