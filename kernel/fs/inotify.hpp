#pragma once

#include <fs/vfs.hpp>

// inotify: file-change notifications (Linux ABI). File managers, desktop
// menus, the App Store and editors watch directories with it. The VFS and
// file system calls report events here; with no watches, every call is a
// cheap early return.
namespace inotify {

constexpr u32 IN_ACCESS = 0x1, IN_MODIFY = 0x2, IN_ATTRIB = 0x4, IN_CLOSE_WRITE = 0x8, IN_CLOSE_NOWRITE = 0x10,
              IN_OPEN = 0x20, IN_MOVED_FROM = 0x40, IN_MOVED_TO = 0x80, IN_CREATE = 0x100, IN_DELETE = 0x200,
              IN_DELETE_SELF = 0x400, IN_MOVE_SELF = 0x800, IN_ISDIR = 0x40000000;

bool active(); // any watches at all?

// Event about the child `name` of directory `dir` (sent to dir's watches).
void child_event(vfs::Inode* dir, u32 mask, const char* name, usize len, u32 cookie = 0);
// Event about `inode` itself (sent to its own watches).
void self_event(vfs::Inode* inode, u32 mask);
// Event on an open file: its own watches, plus its directory's (found via
// the path the file was opened by).
void file_event(vfs::File* f, u32 mask);
// The inode was deleted / is being freed: IN_DELETE_SELF (if watched for),
// then IN_IGNORED, and its watches are dropped.
void inode_gone(vfs::Inode* inode);

u32 next_cookie(); // pairs IN_MOVED_FROM with IN_MOVED_TO

} // namespace inotify
