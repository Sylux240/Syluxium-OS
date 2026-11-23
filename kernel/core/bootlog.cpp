#include <block/block.hpp>
#include <core/bootlog.hpp>
#include <core/log.hpp>
#include <core/smbios.hpp>
#include <boot/limine_requests.hpp>
#include <fs/fat.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>

namespace bootlog {

namespace {

sched::Mutex g_lock;
bool g_logs_mounted = false;

// The SYLUXLOGS partition of a Syluxium stick: plain FAT (type 0x0c, so
// other computers mount it automatically) with our README.TXT.
vfs::Inode* g_logs_root = nullptr; // the SYLUXLOGS volume
vfs::Inode* g_logs_dir = nullptr;  // this boot's folder on it
char g_logs_source[32];
char g_logs_target[96];

// /boot/logs-stick and the /boot/logs link on the current "/".
bool attach_logs(vfs::Inode* root, const char* source, const char* target) {
    vfs::mkdir_p("/boot/logs-stick");
    if (vfs::mount_at("/boot/logs-stick", root, source, "vfat") != 0)
        return false;
    vfs::Inode* bootdir;
    if (vfs::resolve("/boot", nullptr, true, &bootdir) != 0)
        return false;
    bootdir->unlink("logs", 4, false);
    return bootdir->create("logs", 4, vfs::S_IFLNK | 0777, target, nullptr) == 0;
}

void mount_logs_partition() {
    block::for_each([](block::Device* d, void*) {
        if (g_logs_mounted || !fat::probe(d))
            return;
        vfs::Inode* root;
        if (fat::mount(d, &root) != 0)
            return;
        if (root->lookup("EFI", 3))
            return; // the boot partition (already mounted at /boot/efi)
        vfs::Inode* readme = root->lookup("README.TXT", 10);
        char head[32] = {};
        if (!readme || readme->read(0, head, 31) < 20 || strncmp(head, "Syluxium writes its boot log", 28) != 0)
            return;
        // Each boot gets its own folder, "NN-<machine>", so one stick can
        // collect logs from several computers. /boot/logs points into it
        // (the kernel log, display.txt, session logs all land there).
        vfs::mkdir_p("/boot/logs-stick");
        char source[32];
        snprintf(source, sizeof(source), "/dev/%s", d->name);
        if (vfs::mount_at("/boot/logs-stick", root, source, "vfat") != 0)
            return;
        unsigned highest = 0;
        vfs::DirEntry e;
        for (u64 i = 0; root->readdir(i, &e) == 1; i++)
            if (e.type == vfs::DT_DIR && e.name[0] >= '0' && e.name[0] <= '9' && e.name[1] >= '0' &&
                e.name[1] <= '9' && e.name[2] == '-') {
                unsigned n = static_cast<unsigned>((e.name[0] - '0') * 10 + (e.name[1] - '0'));
                highest = n > highest ? n : highest;
            }
        unsigned number = highest < 99 ? highest + 1 : 99; // 99 boots: the last folder is reused
        char machine[48];
        snprintf(machine, sizeof(machine), "%s", smbios::machine_name());
        for (char* c = machine; *c; c++) // FAT can't store these in names
            if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' ||
                *c == '>' || *c == '|')
                *c = '-';
        usize mlen = strlen(machine);
        while (mlen && (machine[mlen - 1] == ' ' || machine[mlen - 1] == '.'))
            machine[--mlen] = 0;
        char folder[64];
        snprintf(folder, sizeof(folder), "%02u-%s", number, mlen ? machine : "Unknown PC");
        vfs::Inode* dir = root->lookup(folder, strlen(folder));
        if (!dir && root->create(folder, strlen(folder), vfs::S_IFDIR | 0755, nullptr, &dir) != 0)
            return;
        char target[96];
        snprintf(target, sizeof(target), "/boot/logs-stick/%s", folder);
        vfs::Inode* bootdir;
        if (vfs::resolve("/boot", nullptr, true, &bootdir) != 0)
            return;
        bootdir->unlink("logs", 4, false);
        if (bootdir->create("logs", 4, vfs::S_IFLNK | 0777, target, nullptr) != 0)
            return;
        g_logs_mounted = true;
        g_logs_root = root;
        g_logs_dir = dir;
        snprintf(g_logs_source, sizeof(g_logs_source), "%s", source);
        snprintf(g_logs_target, sizeof(g_logs_target), "%s", target);
        char info[256];
        int n = snprintf(info, sizeof(info), "Computer: %s\r\nBoot options: %s\r\n", mlen ? machine : "unknown",
                         boot::cmdline());
        vfs::create_file("/boot/logs/machine.txt", info, static_cast<usize>(n), 0644);
        block::sync_all(); // the folder is on the stick before anything else can go wrong
        klog::ok("Mounted %s (SYLUXLOGS): this boot's logs go to %s", source, folder);
    }, nullptr);
}

void writer(void*) {
    // Early and often at first: a machine that freezes soon after still
    // leaves its log (then every few minutes for the rest of the session).
    // Machines under test freeze at any moment (often when a program starts
    // using the GPU): the first 15 minutes are saved every 20 seconds.
    static const u32 at_seconds[] = {2, 5, 10, 15, 20};
    u32 elapsed = 0;
    save();
    for (u32 t : at_seconds) {
        sched::sleep_ms(static_cast<u64>(t - elapsed) * 1000);
        elapsed = t;
        save();
    }
    // Boot option "logfast" (GPU test entries): every second instead, so a
    // machine that freezes when a program first uses the GPU leaves the
    // kernel's last lines (the 20 s rhythm lost them).
    bool fast = false;
    for (const char* c = boot::cmdline(); (c = strstr(c, "logfast")) != nullptr; c += 7)
        if ((c == boot::cmdline() || c[-1] == ' ') && (c[7] == 0 || c[7] == ' '))
            fast = true;
    if (fast) {
        klog::info("Boot logs: saved every second for 15 minutes (logfast)");
        for (u32 ms = elapsed * 1000; ms < 900'000; ms += 1000) {
            sched::sleep_ms(1000);
            save();
        }
        elapsed = 900;
    }
    for (; elapsed < 900; elapsed += 20) {
        sched::sleep_ms(20'000);
        save();
    }
    for (;;) { // the rest of the session: every 2 minutes
        sched::sleep_ms(120'000);
        save();
    }
}

} // namespace

void save() {
    LockGuard guard(g_lock);
    const char* path = "/boot/logs/syluxium-log.txt";
    vfs::Inode* marker;
    if (!g_logs_mounted) {
        // Older sticks: the boot partition itself (only ever our own).
        if (vfs::resolve("/boot/efi/boot/syluxium.elf", nullptr, true, &marker) != 0)
            return;
        path = "/boot/efi/syluxium-log.txt";
    }
    usize len = klog::buffer_length();
    char* text = static_cast<char*>(kmalloc(len + 1));
    if (!text)
        return;
    len = klog::read_buffer(text, len);
    vfs::Inode* file;
    if (g_logs_dir) {
        // Through the folder itself, not the path: "/" can change under us
        // (root= on a disk), the stick's folder doesn't.
        static const char name[] = "syluxium-log.txt";
        file = g_logs_dir->lookup(name, sizeof(name) - 1);
        if (!file && g_logs_dir->create(name, sizeof(name) - 1, vfs::S_IFREG | 0644, nullptr, &file) != 0)
            file = nullptr;
        if (file) {
            file->truncate(0);
            file->write(0, text, len);
        }
    } else if (vfs::resolve(path, nullptr, true, &file) == 0) {
        file->truncate(0);
        file->write(0, text, len);
    } else {
        vfs::create_file(path, text, len, 0644);
    }
    kfree(text);
    block::sync_all();
}

void root_changed() {
    if (g_logs_root && attach_logs(g_logs_root, g_logs_source, g_logs_target))
        klog::info("Boot logs: %s mounted at /boot/logs-stick on the new root", g_logs_source);
}

void start() {
    mount_logs_partition();
    sched::spawn("bootlog", writer, nullptr, -1, true);
}

} // namespace bootlog
