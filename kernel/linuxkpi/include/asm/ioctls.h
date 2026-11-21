/* LinuxKPI: terminal/socket ioctl numbers. */
#pragma once
#define TIOCOUTQ 0x5411
#define TIOCINQ 0x541B
#ifndef FIONREAD
#define FIONREAD 0x541B
#endif
#define SIOCGSTAMP_OLD 0x8906
#define SIOCGSTAMPNS_OLD 0x8907
