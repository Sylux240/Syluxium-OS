// FUSE: a small file system served over /dev/fuse with the raw protocol
// (<linux/fuse.h>, no libfuse), the way libfuse and fusermount3 drive it:
//   1. mount(2) "fuse.fusetest" with fd=,rootmode=,user_id=,group_id=; /proc/mounts lists it
//   2. stat, read, readdir, readlink, statfs through the server
//   3. a private mapping of a multi-page file and exec of a program served by it
//      (what an AppImage's runtime does: programs and libraries from squashfuse)
//   4. create, write, read back, rename, chmod, truncate, mkdir, rmdir, unlink
//   5. umount: the server's read() returns ENODEV and it exits
//   6. the server killed while mounted: ENOTCONN, and umount still works
// Needs root. Leaves nothing mounted behind.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("fusetest: FAIL: " __VA_ARGS__); printf(" (%s)\n", strerror(errno)); failures++; } } while (0)

#define MNT "/tmp/fusetest"
#define HELLO "Hello, FUSE!\n"
#define BIG_SIZE (3 * 4096 + 1234)

// ---- The server's file system: a flat table of nodes ----
struct node {
    uint64_t id, parent;
    char name[64];
    uint32_t mode;
    char* data;
    size_t size;
    int used;
};
#define MAX_NODES 32
static struct node nodes[MAX_NODES];
static uint64_t next_id = 1;

static struct node* add(uint64_t parent, const char* name, uint32_t mode, const void* data, size_t size) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].used)
            continue;
        struct node* n = &nodes[i];
        memset(n, 0, sizeof(*n));
        n->used = 1;
        n->id = next_id++;
        n->parent = parent;
        snprintf(n->name, sizeof(n->name), "%s", name);
        n->mode = mode;
        if (size) {
            n->data = malloc(size);
            memcpy(n->data, data, size);
        }
        n->size = size;
        return n;
    }
    return NULL;
}

static struct node* by_id(uint64_t id) {
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].used && nodes[i].id == id)
            return &nodes[i];
    return NULL;
}

static struct node* by_name(uint64_t parent, const char* name) {
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].used && nodes[i].parent == parent && nodes[i].id != parent && strcmp(nodes[i].name, name) == 0)
            return &nodes[i];
    return NULL;
}

static void fill_attr(struct node* n, struct fuse_attr* a) {
    memset(a, 0, sizeof(*a));
    a->ino = n->id;
    a->size = n->size;
    a->blocks = (n->size + 511) / 512;
    a->mode = n->mode;
    a->nlink = S_ISDIR(n->mode) ? 2 : 1;
    a->mtime = a->atime = a->ctime = 1700000000;
    a->blksize = 4096;
}

static void build_tree(const char* prog_path) {
    struct node* root = add(1, "", S_IFDIR | 0755, NULL, 0); // id 1 = FUSE_ROOT_ID
    root->parent = 1;
    add(1, "hello.txt", S_IFREG | 0644, HELLO, strlen(HELLO));
    add(1, "link", S_IFLNK | 0777, "hello.txt", 9);
    struct node* sub = add(1, "sub", S_IFDIR | 0755, NULL, 0);
    add(sub->id, "inner.txt", S_IFREG | 0644, "inner\n", 6);
    char* big = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; i++)
        big[i] = (char)(i * 7 + i / 4096);
    add(1, "big.bin", S_IFREG | 0644, big, BIG_SIZE);
    free(big);
    // A real program, served from memory.
    int fd = open(prog_path, O_RDONLY);
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0) {
        char* buf = malloc(st.st_size);
        if (read(fd, buf, st.st_size) == st.st_size)
            add(1, "prog", S_IFREG | 0755, buf, st.st_size);
        free(buf);
    }
    if (fd >= 0)
        close(fd);
}

static void reply(int fd, uint64_t unique, int error, const void* data, size_t len) {
    char buf[sizeof(struct fuse_out_header) + 65536 + 4096];
    struct fuse_out_header* h = (struct fuse_out_header*)buf;
    if (error)
        len = 0;
    h->len = sizeof(*h) + len;
    h->error = error;
    h->unique = unique;
    if (len)
        memcpy(buf + sizeof(*h), data, len);
    if (write(fd, buf, h->len) != (ssize_t)h->len)
        fprintf(stderr, "fusetest server: reply write failed: %s\n", strerror(errno));
}

static void reply_entry(int fd, uint64_t unique, struct node* n) {
    struct fuse_entry_out e;
    memset(&e, 0, sizeof(e));
    e.nodeid = n->id;
    e.entry_valid = 1;
    e.attr_valid = 1;
    fill_attr(n, &e.attr);
    reply(fd, unique, 0, &e, sizeof(e));
}

static void reply_attr(int fd, uint64_t unique, struct node* n) {
    struct fuse_attr_out a;
    memset(&a, 0, sizeof(a));
    a.attr_valid = 1;
    fill_attr(n, &a.attr);
    reply(fd, unique, 0, &a, sizeof(a));
}

// Serve until the kernel says the file system is gone. Returns 0 on a clean end.
static int serve(int fd) {
    static char buf[FUSE_MIN_READ_BUFFER + 131072];
    int initialized = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            if (errno == ENODEV)
                return initialized ? 0 : 3; // unmounted
            fprintf(stderr, "fusetest server: read: %s\n", strerror(errno));
            return 1;
        }
        if ((size_t)n < sizeof(struct fuse_in_header)) {
            fprintf(stderr, "fusetest server: short request (%zd bytes)\n", n);
            return 2;
        }
        struct fuse_in_header* in = (struct fuse_in_header*)buf;
        void* arg = buf + sizeof(*in);
        struct node* node = by_id(in->nodeid);
        switch (in->opcode) {
        case FUSE_INIT: {
            struct fuse_init_in* ii = arg;
            struct fuse_init_out o;
            memset(&o, 0, sizeof(o));
            o.major = FUSE_KERNEL_VERSION;
            o.minor = ii->minor < FUSE_KERNEL_MINOR_VERSION ? ii->minor : FUSE_KERNEL_MINOR_VERSION;
            o.max_readahead = ii->max_readahead;
            o.max_write = 65536;
            o.flags = FUSE_BIG_WRITES & ii->flags;
            initialized = 1;
            reply(fd, in->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_LOOKUP: {
            struct node* c = node ? by_name(node->id, (const char*)arg) : NULL;
            if (c)
                reply_entry(fd, in->unique, c);
            else
                reply(fd, in->unique, -ENOENT, NULL, 0);
            break;
        }
        case FUSE_FORGET:
        case FUSE_BATCH_FORGET:
            break; // no reply
        case FUSE_GETATTR:
            if (node)
                reply_attr(fd, in->unique, node);
            else
                reply(fd, in->unique, -ENOENT, NULL, 0);
            break;
        case FUSE_SETATTR: {
            struct fuse_setattr_in* s = arg;
            if (!node) {
                reply(fd, in->unique, -ENOENT, NULL, 0);
                break;
            }
            if (s->valid & FATTR_MODE)
                node->mode = (node->mode & S_IFMT) | (s->mode & 07777);
            if (s->valid & FATTR_SIZE) {
                node->data = realloc(node->data, s->size ? s->size : 1);
                if (s->size > node->size)
                    memset(node->data + node->size, 0, s->size - node->size);
                node->size = s->size;
            }
            reply_attr(fd, in->unique, node);
            break;
        }
        case FUSE_READLINK:
            if (node && S_ISLNK(node->mode))
                reply(fd, in->unique, 0, node->data, node->size);
            else
                reply(fd, in->unique, -EINVAL, NULL, 0);
            break;
        case FUSE_MKNOD:
        case FUSE_MKDIR: {
            uint32_t mode;
            const char* name;
            if (in->opcode == FUSE_MKNOD) {
                mode = ((struct fuse_mknod_in*)arg)->mode;
                name = (const char*)arg + sizeof(struct fuse_mknod_in);
            } else {
                mode = S_IFDIR | (((struct fuse_mkdir_in*)arg)->mode & 07777);
                name = (const char*)arg + sizeof(struct fuse_mkdir_in);
            }
            struct node* c = node && !by_name(node->id, name) ? add(node->id, name, mode, NULL, 0) : NULL;
            if (c)
                reply_entry(fd, in->unique, c);
            else
                reply(fd, in->unique, -EEXIST, NULL, 0);
            break;
        }
        case FUSE_UNLINK:
        case FUSE_RMDIR: {
            struct node* c = node ? by_name(node->id, (const char*)arg) : NULL;
            if (!c) {
                reply(fd, in->unique, -ENOENT, NULL, 0);
                break;
            }
            free(c->data);
            c->used = 0;
            reply(fd, in->unique, 0, NULL, 0);
            break;
        }
        case FUSE_RENAME: {
            struct fuse_rename_in* r = arg;
            const char* oldname = (const char*)(r + 1);
            const char* newname = oldname + strlen(oldname) + 1;
            struct node* c = node ? by_name(node->id, oldname) : NULL;
            if (!c) {
                reply(fd, in->unique, -ENOENT, NULL, 0);
                break;
            }
            struct node* victim = by_name(r->newdir, newname);
            if (victim) {
                free(victim->data);
                victim->used = 0;
            }
            c->parent = r->newdir;
            snprintf(c->name, sizeof(c->name), "%s", newname);
            reply(fd, in->unique, 0, NULL, 0);
            break;
        }
        case FUSE_OPEN:
        case FUSE_OPENDIR: {
            struct fuse_open_out o;
            memset(&o, 0, sizeof(o));
            o.fh = in->nodeid + 1000;
            reply(fd, in->unique, node ? 0 : -ENOENT, &o, sizeof(o));
            break;
        }
        case FUSE_READ: {
            struct fuse_read_in* r = arg;
            if (!node || r->fh != node->id + 1000) {
                reply(fd, in->unique, -EBADF, NULL, 0);
                break;
            }
            size_t len = 0;
            if (r->offset < node->size)
                len = node->size - r->offset < r->size ? node->size - r->offset : r->size;
            reply(fd, in->unique, 0, node->data + r->offset, len);
            break;
        }
        case FUSE_WRITE: {
            struct fuse_write_in* w = arg;
            const char* data = (const char*)(w + 1);
            if (!node) {
                reply(fd, in->unique, -EBADF, NULL, 0);
                break;
            }
            if (w->offset + w->size > node->size) {
                node->data = realloc(node->data, w->offset + w->size);
                memset(node->data + node->size, 0, w->offset + w->size - node->size);
                node->size = w->offset + w->size;
            }
            memcpy(node->data + w->offset, data, w->size);
            struct fuse_write_out o = {.size = w->size};
            reply(fd, in->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_STATFS: {
            struct fuse_statfs_out o;
            memset(&o, 0, sizeof(o));
            o.st.blocks = 1000;
            o.st.bfree = o.st.bavail = 500;
            o.st.files = MAX_NODES;
            o.st.bsize = o.st.frsize = 4096;
            o.st.namelen = 63;
            reply(fd, in->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_READDIR: {
            struct fuse_read_in* r = arg;
            char out[8192];
            size_t used = 0;
            uint64_t index = 0;
            for (int i = 0; i < MAX_NODES && node; i++) {
                struct node* c = &nodes[i];
                if (!c->used || c->parent != node->id || c->id == node->id)
                    continue;
                index++;
                if (index <= r->offset)
                    continue;
                size_t namelen = strlen(c->name);
                size_t rec = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + namelen);
                if (used + rec > r->size || used + rec > sizeof(out))
                    break;
                struct fuse_dirent* d = (struct fuse_dirent*)(out + used);
                memset(d, 0, rec);
                d->ino = c->id;
                d->off = index;
                d->namelen = namelen;
                d->type = (c->mode & S_IFMT) >> 12;
                memcpy(d->name, c->name, namelen);
                used += rec;
            }
            reply(fd, in->unique, 0, out, used);
            break;
        }
        case FUSE_RELEASE:
        case FUSE_RELEASEDIR:
        case FUSE_FLUSH:
        case FUSE_DESTROY:
            reply(fd, in->unique, 0, NULL, 0);
            break;
        default:
            reply(fd, in->unique, -ENOSYS, NULL, 0);
            break;
        }
    }
}

// Mount a fresh connection on MNT and start its server; returns the server's pid.
static pid_t start(void) {
    int fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("fusetest: FAIL: open /dev/fuse (%s)\n", strerror(errno));
        failures++;
        return -1;
    }
    char opts[128];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0", fd);
    if (mount("fusetest", MNT, "fuse.fusetest", MS_NOSUID | MS_NODEV, opts) != 0) {
        printf("fusetest: FAIL: mount (%s)\n", strerror(errno));
        failures++;
        close(fd);
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0)
        _exit(serve(fd));
    close(fd);
    return pid;
}

static int read_file(const char* path, char* buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return (int)n;
}

int main(int argc, char** argv) {
    build_tree(argc > 1 ? argv[1] : "/opt/syluxium/tests/hello");
    mkdir(MNT, 0755);
    pid_t server = start();
    if (server < 0)
        return 1;

    // 1. /proc/mounts
    char buf[65536];
    CHECK(read_file("/proc/mounts", buf, sizeof(buf)) > 0 && strstr(buf, "fusetest " MNT " fuse.fusetest ") &&
              strstr(buf, "user_id=0,group_id=0"), "/proc/mounts entry");

    // 2. stat, read, readdir, readlink, statfs
    struct stat st;
    CHECK(stat(MNT "/hello.txt", &st) == 0 && st.st_size == (off_t)strlen(HELLO) && S_ISREG(st.st_mode) &&
              (st.st_mode & 07777) == 0644, "stat hello.txt");
    CHECK(read_file(MNT "/hello.txt", buf, sizeof(buf)) == (int)strlen(HELLO) && strcmp(buf, HELLO) == 0, "read hello.txt");
    CHECK(read_file(MNT "/sub/inner.txt", buf, sizeof(buf)) == 6 && strcmp(buf, "inner\n") == 0, "read sub/inner.txt");
    CHECK(read_file(MNT "/link", buf, sizeof(buf)) > 0 && strcmp(buf, HELLO) == 0, "read through the symlink");
    ssize_t ln = readlink(MNT "/link", buf, sizeof(buf));
    CHECK(ln == 9 && memcmp(buf, "hello.txt", 9) == 0, "readlink");
    CHECK(stat(MNT "/missing", &st) != 0 && errno == ENOENT, "missing name gives ENOENT");
    DIR* d = opendir(MNT);
    int seen = 0, dots = 0;
    struct dirent* e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            dots++;
        else if (!strcmp(e->d_name, "hello.txt") && e->d_type == DT_REG)
            seen |= 1;
        else if (!strcmp(e->d_name, "sub") && e->d_type == DT_DIR)
            seen |= 2;
        else if (!strcmp(e->d_name, "link") && e->d_type == DT_LNK)
            seen |= 4;
    }
    if (d)
        closedir(d);
    CHECK(seen == 7 && dots == 2, "readdir (seen %d, dots %d)", seen, dots);
    struct statfs sf;
    CHECK(statfs(MNT, &sf) == 0 && sf.f_type == 0x65735546 && sf.f_bsize == 4096 && sf.f_blocks == 1000, "statfs");

    // 3. a private mapping over several pages, and exec from the mount
    int fd = open(MNT "/big.bin", O_RDONLY);
    char* map = fd >= 0 ? mmap(NULL, BIG_SIZE, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    int good = map != MAP_FAILED;
    for (int i = 0; good && i < BIG_SIZE; i++)
        good = map[i] == (char)(i * 7 + i / 4096);
    CHECK(good, "mmap of big.bin matches");
    if (map != MAP_FAILED)
        munmap(map, BIG_SIZE);
    if (fd >= 0) {
        CHECK(pread(fd, buf, 100, 4090) == 100 && buf[0] == (char)(4090 * 7) && buf[10] == (char)(4100 * 7 + 1),
              "pread across a page boundary");
        close(fd);
    }
    pid_t child = fork();
    if (child == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1);
        execl(MNT "/prog", "prog", (char*)NULL);
        _exit(127);
    }
    int status = -1;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "exec a program from the mount (status %#x)", status);

    // 4. writing
    fd = open(MNT "/new.txt", O_CREAT | O_WRONLY | O_TRUNC, 0640);
    CHECK(fd >= 0 && write(fd, "abc", 3) == 3, "create + write");
    if (fd >= 0)
        close(fd);
    CHECK(read_file(MNT "/new.txt", buf, sizeof(buf)) == 3 && strcmp(buf, "abc") == 0, "read back what was written");
    fd = open(MNT "/new.txt", O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "def", 3) == 3, "append");
    if (fd >= 0)
        close(fd);
    CHECK(read_file(MNT "/new.txt", buf, sizeof(buf)) == 6 && strcmp(buf, "abcdef") == 0, "appended data");
    CHECK(rename(MNT "/new.txt", MNT "/sub/moved.txt") == 0, "rename into a subdirectory");
    CHECK(stat(MNT "/new.txt", &st) != 0 && errno == ENOENT, "old name gone after rename");
    CHECK(chmod(MNT "/sub/moved.txt", 0600) == 0 && stat(MNT "/sub/moved.txt", &st) == 0 && (st.st_mode & 07777) == 0600,
          "chmod");
    CHECK(truncate(MNT "/sub/moved.txt", 2) == 0 && stat(MNT "/sub/moved.txt", &st) == 0 && st.st_size == 2, "truncate");
    CHECK(mkdir(MNT "/newdir", 0755) == 0 && stat(MNT "/newdir", &st) == 0 && S_ISDIR(st.st_mode), "mkdir");
    CHECK(rmdir(MNT "/newdir") == 0 && stat(MNT "/newdir", &st) != 0, "rmdir");
    CHECK(unlink(MNT "/sub/moved.txt") == 0 && stat(MNT "/sub/moved.txt", &st) != 0 && errno == ENOENT, "unlink");

    // 5. umount ends the server cleanly
    CHECK(umount2(MNT, MNT_DETACH) == 0, "umount");
    status = -1;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server ends with ENODEV after umount (status %#x)", status);
    CHECK(read_file("/proc/mounts", buf, sizeof(buf)) > 0 && !strstr(buf, " " MNT " "), "gone from /proc/mounts");

    // 6. the server dies while mounted
    server = start();
    if (server > 0) {
        CHECK(stat(MNT "/hello.txt", &st) == 0, "second mount works");
        kill(server, SIGKILL);
        waitpid(server, NULL, 0);
        CHECK(stat(MNT, &st) != 0 && errno == ENOTCONN, "ENOTCONN once the server is gone");
        CHECK(umount2(MNT, MNT_DETACH) == 0, "umount after the server died");
    }
    rmdir(MNT);

    if (failures == 0)
        printf("fusetest: all FUSE checks passed\n");
    return failures ? 1 : 0;
}
