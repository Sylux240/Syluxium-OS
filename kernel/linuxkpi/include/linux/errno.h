#ifndef _LKPI_LINUX_ERRNO_H
#define _LKPI_LINUX_ERRNO_H

/* Linux errno numbers (the same values the syscall ABI uses). */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define EWOULDBLOCK EAGAIN
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define EDEADLK 35
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define ENOMSG 42
#define ENODATA 61
#define ETIME 62
#define ENOLINK 67
#define EPROTO 71
#define EBADMSG 74
#define EOVERFLOW 75
#define EILSEQ 84
#define EUSERS 87
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define ENOTSUPP 524
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ENETRESET 102
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTDOWN 112
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define ECANCELED 125
#define ENOKEY 126
#define EKEYEXPIRED 127
#define ERFKILL 132
#define ERESTARTSYS 512
#define EPROBE_DEFER 517

#define EL2NSYNC 45
#define EBADE 52
#define EBADR 53
#define EXFULL 54
#define ENOANO 55
#define EBADRQC 56
#define EBADSLT 57
#define ENOSTR 60
#define ENOSR 63
#define ENONET 64
#define ECOMM 70
#define EREMCHG 78
#define ESOCKTNOSUPPORT 94
#define EPFNOSUPPORT 96
#define ESHUTDOWN 108
#define ETOOMANYREFS 109
#define ESTALE 116
#define EUCLEAN 117
#define ENAVAIL 119
#define EREMOTEIO 121
#define EDQUOT 122
#define ENOMEDIUM 123
#define EMEDIUMTYPE 124
#define EKEYREVOKED 128
#define EKEYREJECTED 129
#define EOWNERDEAD 130
#define ENOTRECOVERABLE 131
#define EHWPOISON 133
#define ENOTUNIQ 76
#define EBADFD 77
#define ERESTARTNOINTR 513
#define ERESTARTNOHAND 514
#define ENOIOCTLCMD 515
#define ERESTART_RESTARTBLOCK 516
#define EBADHANDLE 521
#define ENOTSYNC 522
#define EBADCOOKIE 523
#define ETOOSMALL 525
#define ESERVERFAULT 526
#define EBADTYPE 527
#define EJUKEBOX 528
#define EIOCBQUEUED 529

/* The rest of Linux errno numbers. */
#ifndef ENOTBLK
#define ENOTBLK 15
#endif
#ifndef ETXTBSY
#define ETXTBSY 26
#endif
#ifndef EMLINK
#define EMLINK 31
#endif
#ifndef ENOLCK
#define ENOLCK 37
#endif
#ifndef EIDRM
#define EIDRM 43
#endif
#ifndef ECHRNG
#define ECHRNG 44
#endif
#ifndef EL3HLT
#define EL3HLT 46
#endif
#ifndef EL3RST
#define EL3RST 47
#endif
#ifndef ELNRNG
#define ELNRNG 48
#endif
#ifndef EUNATCH
#define EUNATCH 49
#endif
#ifndef ENOCSI
#define ENOCSI 50
#endif
#ifndef EL2HLT
#define EL2HLT 51
#endif
#ifndef EDEADLOCK
#define EDEADLOCK EDEADLK
#endif
#ifndef EBFONT
#define EBFONT 59
#endif
#ifndef ENOPKG
#define ENOPKG 65
#endif
#ifndef EREMOTE
#define EREMOTE 66
#endif
#ifndef EADV
#define EADV 68
#endif
#ifndef ESRMNT
#define ESRMNT 69
#endif
#ifndef EMULTIHOP
#define EMULTIHOP 72
#endif
#ifndef EDOTDOT
#define EDOTDOT 73
#endif
#ifndef ELIBACC
#define ELIBACC 79
#endif
#ifndef ELIBBAD
#define ELIBBAD 80
#endif
#ifndef ELIBSCN
#define ELIBSCN 81
#endif
#ifndef ELIBMAX
#define ELIBMAX 82
#endif
#ifndef ELIBEXEC
#define ELIBEXEC 83
#endif
#ifndef ERESTART
#define ERESTART 85
#endif
#ifndef ESTRPIPE
#define ESTRPIPE 86
#endif
#ifndef ENOTNAM
#define ENOTNAM 118
#endif
#ifndef EISNAM
#define EISNAM 120
#endif
#ifndef EOPENSTALE
#define EOPENSTALE 518
#endif
#ifndef ENOPARAM
#define ENOPARAM 519
#endif
#ifndef ERECALLCONFLICT
#define ERECALLCONFLICT 530
#endif
#ifndef ENOGRACE
#define ENOGRACE 531
#endif
#endif
