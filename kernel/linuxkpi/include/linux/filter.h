/* LinuxKPI: socket filters (BPF) are not run. */
#pragma once
#include <linux/lkpi_all.h>
struct sock_filter { u16 code; u8 jt; u8 jf; u32 k; };
struct sock_fprog { unsigned short len; struct sock_filter __user *filter; };
