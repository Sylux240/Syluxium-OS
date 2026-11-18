#pragma once

#include <fs/vfs.hpp>

// Anonymous pipes (pipe/pipe2): a 64 KiB ring buffer between a read end and
// a write end, with POSIX semantics (blocking, EOF when all writers close,
// SIGPIPE/EPIPE when all readers close, atomic writes up to PIPE_BUF).
namespace pipe {

// Create a pipe; returns the read and write ends.
long create(u32 flags, vfs::File** read_end, vfs::File** write_end);

// The pipe behind a named FIFO node (mkfifo/mknod), on any file system:
// every open of the node shares it.
vfs::Inode* fifo_for(vfs::Inode* node);

} // namespace pipe
