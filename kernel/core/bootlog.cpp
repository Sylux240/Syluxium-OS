#include <block/block.hpp>
#include <core/bootlog.hpp>
#include <core/log.hpp>
#include <fs/vfs.hpp>
#include <mm/heap.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>

namespace bootlog {

namespace {

sched::Mutex g_lock;

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
    vfs::Inode* marker;
    if (vfs::resolve("/boot/efi/boot/syluxium.elf", nullptr, true, &marker) != 0)
        return; // not booted from a Syluxium boot partition we can see
    usize len = klog::buffer_length();
    char* text = static_cast<char*>(kmalloc(len + 1));
    if (!text)
        return;
    len = klog::read_buffer(text, len);
    const char* path = "/boot/efi/syluxium-log.txt";
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

void start() { sched::spawn("bootlog", writer, nullptr, -1, true); }

} // namespace bootlog
