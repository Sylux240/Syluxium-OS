#include <block/block.hpp>
#include <core/bootlog.hpp>
#include <core/log.hpp>
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
        vfs::mkdir_p("/boot/logs");
        char source[32];
        snprintf(source, sizeof(source), "/dev/%s", d->name);
        if (vfs::mount_at("/boot/logs", root, source, "vfat") == 0) {
            g_logs_mounted = true;
            klog::ok("Mounted %s (SYLUXLOGS) on /boot/logs: the boot log is saved there", source);
        }
    }, nullptr);
}

void writer(void*) {
    static const u32 at_seconds[] = {10, 20, 30, 45, 60, 90, 120, 180, 300, 600};
    u32 elapsed = 0;
    for (u32 t : at_seconds) {
        sched::sleep_ms(static_cast<u64>(t - elapsed) * 1000);
        elapsed = t;
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
    if (vfs::resolve(path, nullptr, true, &file) == 0) {
        file->truncate(0);
        file->write(0, text, len);
    } else {
        vfs::create_file(path, text, len, 0644);
    }
    kfree(text);
    block::sync_all();
}

void start() {
    mount_logs_partition();
    sched::spawn("bootlog", writer, nullptr, -1, true);
}

} // namespace bootlog
