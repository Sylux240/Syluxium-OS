#pragma once

// Linux errno values (the syscall ABI returns -errno). These numbers are
// part of the Linux ABI and must not change.
namespace err {

constexpr long PERM = 1, NOENT = 2, SRCH = 3, INTR = 4, IO = 5, NXIO = 6, TOOBIG = 7,
               NOEXEC = 8, BADF = 9, CHILD = 10, AGAIN = 11, NOMEM = 12, ACCES = 13,
               FAULT = 14, BUSY = 16, EXIST = 17, XDEV = 18, NODEV = 19, NOTDIR = 20,
               ISDIR = 21, INVAL = 22, NFILE = 23, MFILE = 24, NOTTY = 25, FBIG = 27,
               NOSPC = 28, SPIPE = 29, ROFS = 30, MLINK = 31, PIPE = 32, RANGE = 34,
               NAMETOOLONG = 36, NOSYS = 38, NOTEMPTY = 39, LOOP = 40, NODATA = 61,
               OVERFLOW = 75, OPNOTSUPP = 95, TIMEDOUT = 110;

} // namespace err
