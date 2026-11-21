#include <core/watchdog.hpp>
#include <core/bootlog.hpp>
#include <power/cpupower.hpp>
#include <acpi/acpi.hpp>
#include <acpi/power.hpp>
#include <acpi/devices.hpp>
#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <arch/x86_64/pic.hpp>
#include <arch/x86_64/smp.hpp>
#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <cxx/runtime.hpp>
#include <dev/device.hpp>
#include <dev/platform.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/input/input.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/usb/usb.hpp>
#include <linuxkpi/linuxkpi.hpp>
#include <video/fbdev.hpp>
#include <net/net.hpp>
#include <drivers/storage/storage.hpp>
#include <drivers/sound/sound_drivers.hpp>
#include <sound/sound.hpp>
#include <block/block.hpp>
#include <fs/ext2.hpp>
#include <fs/fat.hpp>
#include <drivers/serial.hpp>
#include <drivers/tty.hpp>
#include <fs/devices.hpp>
#include <fs/initramfs.hpp>
#include <fs/procfs.hpp>
#include <fs/vfs.hpp>
#include <lib/random.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <mm/vmalloc.hpp>
#include <mm/vmm.hpp>
#include <proc/process.hpp>
#include <sched/sched.hpp>
#include <syscall/syscall.hpp>
#include <tests/selftest.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>
#include <video/console.hpp>
#include <video/framebuffer.hpp>
#include <video/splash.hpp>

extern "C" const char syluxium_build_id[]; // generated at link time (Makefile)

namespace {

void init_display() {
    auto* fbs = boot::framebuffers();
    if (!fbs || fbs->framebuffer_count == 0)
        return; // headless: serial only
    limine_framebuffer* fb = fbs->framebuffers[0];
    auto& primary = video::primary_framebuffer();
    if (primary.init(fb->address, fb->width, fb->height, fb->pitch, fb->bpp,
                     fb->red_mask_shift, fb->green_mask_shift, fb->blue_mask_shift))
        console::init(primary);
    splash::show(primary); // logo + load bar until Esc, X, or the shell
    // The modes the firmware offers (Display Settings picks one for the next boot).
    if (fbs->revision >= 1)
        for (u64 i = 0; i < fb->mode_count; i++)
            fbdev::record_mode(static_cast<u32>(fb->modes[i]->width), static_cast<u32>(fb->modes[i]->height),
                               fb->modes[i]->bpp);
}

void print_banner() {
    kprintf("\x1b[96m"
            "   ____         _              _\n"
            "  / ___| _   _ | | _   _ __  _(_)_   _ _ __ ___\n"
            "  \\___ \\| | | || || | | |\\ \\/ / | | | | '_ ` _ \\\n"
            "   ___) | |_| || || |_| | >  <| | |_| | | | | | |\n"
            "  |____/ \\__, ||_| \\__,_|/_/\\_\\_|\\__,_|_| |_| |_|\n"
            "         |___/\x1b[0m\n"
            "  \x1b[90mx86_64 kernel, built " __DATE__ " " __TIME__ "\x1b[0m\n\n");
}

void print_cpu_info() {
    auto leaf0 = arch::cpuid(0);
    char vendor[13];
    memcpy(vendor + 0, &leaf0.ebx, 4);
    memcpy(vendor + 4, &leaf0.edx, 4);
    memcpy(vendor + 8, &leaf0.ecx, 4);
    vendor[12] = 0;

    char brand[49] = {};
    if (arch::cpuid(0x80000000).eax >= 0x80000004) {
        for (u32 i = 0; i < 3; i++) {
            auto r = arch::cpuid(0x80000002 + i);
            memcpy(brand + i * 16 + 0, &r.eax, 4);
            memcpy(brand + i * 16 + 4, &r.ebx, 4);
            memcpy(brand + i * 16 + 8, &r.ecx, 4);
            memcpy(brand + i * 16 + 12, &r.edx, 4);
        }
    }
    const char* name = brand;
    while (*name == ' ')
        name++;

    u64 cpus = boot::mp() ? boot::mp()->cpu_count : 1;
    klog::info("Syluxium build %s", syluxium_build_id);
    klog::info("CPU: %s (%s), %lu logical processor(s)", *name ? name : "unknown", vendor, cpus);
}

void print_memory_map() {
    auto* mm = boot::memory_map();
    if (!mm)
        panic("bootloader did not provide a memory map");

    static const char* const type_names[] = {
        "usable", "reserved", "ACPI reclaimable", "ACPI NVS", "bad memory",
        "bootloader reclaimable", "kernel/modules", "framebuffer", "reserved (mapped)",
    };

    // Full map goes to serial only; it doesn't fit on screen on UEFI machines.
    u64 usable = 0;
    klog::info("Memory map: %lu entries (details on serial)", mm->entry_count);
    for (u64 i = 0; i < mm->entry_count; i++) {
        limine_memmap_entry* e = mm->entries[i];
        const char* type = e->type < array_size(type_names) ? type_names[e->type] : "unknown";
        kprintf_serial("    %016lx - %016lx  %8lu KiB  %s\n", e->base, e->base + e->length, e->length / KiB, type);
        if (e->type == LIMINE_MEMMAP_USABLE)
            usable += e->length;
    }
    klog::info("Usable RAM: %lu MiB", usable / MiB);
}

bool g_keyboard = false;
bool g_data_mounted = false;
block::Device* g_root_dev = nullptr; // the disk "/" lives on (root= on the command line)

// The value of `key=` on the kernel command line, or false.
bool cmdline_value(const char* key, char* out, usize size) {
    const char* c = boot::cmdline();
    usize klen = strlen(key);
    for (const char* p = c; *p;) {
        while (*p == ' ')
            p++;
        const char* end = p;
        while (*end && *end != ' ')
            end++;
        if (static_cast<usize>(end - p) > klen && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            usize n = min<usize>(static_cast<usize>(end - p) - klen - 1, size - 1);
            memcpy(out, p + klen + 1, n);
            out[n] = 0;
            return true;
        }
        p = end;
    }
    return false;
}

// Does block device `d` match a root= spec: /dev/NAME, LABEL=..., UUID=...?
bool matches_root_spec(block::Device* d, const char* spec) {
    if (memcmp(spec, "/dev/", 5) == 0)
        return strcmp(d->name, spec + 5) == 0;
    u8 sb[136];
    if (block::read(d, 1024, sb, sizeof(sb)) < 0 || sb[56] != 0x53 || sb[57] != 0xEF)
        return false; // only ext2/3/4 roots
    if (memcmp(spec, "LABEL=", 6) == 0) {
        char label[17] = {};
        memcpy(label, sb + 120, 16);
        return strcmp(label, spec + 6) == 0;
    }
    if (memcmp(spec, "UUID=", 5) == 0) {
        char uuid[37];
        const u8* u = sb + 104;
        snprintf(uuid, sizeof(uuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],
                 u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        const char* w = spec + 5;
        for (usize i = 0; i < 36; i++) {
            char c = w[i];
            if (c >= 'A' && c <= 'F')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != uuid[i])
                return false;
        }
        return w[36] == 0;
    }
    return false;
}

// root=<spec> on the command line: "/" becomes that ext2/3/4 volume (an
// installed Syluxium) instead of the RAM file system. The RAM file system's
// /dev, /proc, /sys, /run and /tmp are mounted on it, as on Linux.
void mount_disk_root() {
    char spec[80];
    if (!cmdline_value("root", spec, sizeof(spec)))
        return;
    struct Find {
        const char* spec;
        block::Device* dev;
    } find{spec, nullptr};
    for (int tries = 0; tries < 300 && !find.dev; tries++) { // up to 30 s for slow (USB) disks and hubs
        block::for_each([](block::Device* d, void* ctx) {
            auto* f = static_cast<Find*>(ctx);
            if (!f->dev && matches_root_spec(d, f->spec))
                f->dev = d;
        }, &find);
        if (!find.dev)
            sched::sleep_ms(100);
    }
    if (!find.dev) {
        klog::error("root=%s: no such disk; staying on the RAM file system", spec);
        return;
    }
    vfs::Inode* root;
    bool ro;
    const char* kind;
    if (long r = ext2::mount(find.dev, &root, &ro, &kind); r < 0) {
        klog::error("root=%s: can't mount /dev/%s (error %ld)", spec, find.dev->name, r);
        return;
    }
    // Directories that stay in RAM (and keep their contents: /dev nodes, /proc, /sys).
    static const char* const ram_dirs[] = {"dev", "proc", "sys", "run", "tmp"};
    for (const char* d : ram_dirs) {
        char path[16];
        snprintf(path, sizeof(path), "/%s", d);
        vfs::mkdir_p(path); // still on the RAM root here
        if (!root->lookup(d, strlen(d)) && !ro)
            root->create(d, strlen(d), vfs::S_IFDIR | 0755, nullptr, nullptr);
    }
    vfs::Inode* ram = vfs::ram_root();
    char source[32];
    snprintf(source, sizeof(source), "/dev/%s", find.dev->name);
    vfs::set_root(root, source, kind);
    g_root_dev = find.dev;
    static const char* const types[] = {"devtmpfs", "proc", "sysfs", "tmpfs", "tmpfs"};
    for (usize i = 0; i < 5; i++) {
        const char* d = ram_dirs[i];
        char path[16];
        snprintf(path, sizeof(path), "/%s", d);
        vfs::Inode* dir = ram->lookup(d, strlen(d));
        if (dir && vfs::mount_at(path, dir, types[i][0] == 't' ? "tmpfs" : types[i], types[i]) < 0)
            klog::warn("root: /%s missing on %s; not mounted", d, source);
    }
    if (!root->lookup("etc", 3) || !ro) {
        vfs::Inode* etc;
        if (vfs::resolve("/etc", nullptr, true, &etc) == 0 && !etc->lookup("mtab", 4))
            etc->create("mtab", 4, vfs::S_IFLNK | 0777, "/proc/mounts", nullptr);
    }
    klog::ok("Root file system: %s (%s%s)", source, kind, ro ? ", read-only" : "");
    if (!ro)
        bootlog::root_changed();
}

// Phase 5: find the hardware, bind drivers, publish it all in /sys.
void discover_hardware() {
    block::init();
    ahci::register_driver();
    nvme::register_driver();
    xhci::register_driver();
    ehci::register_driver();
    uhci::register_driver();
    ohci::register_driver();
    net::init(); // loopback first: it becomes ifindex 1, like Linux
    e1000::register_driver();
    virtio_net::register_driver();
    r8169::register_driver();
    sound::init(); // phase 8: ALSA-compatible sound core
    hda::register_driver();
    virtio_snd::register_driver();
    linuxkpi::init(); // Linux drivers register themselves (module_init) here
    pci::init();
    dev::register_platform_devices(g_keyboard);
    // Disks and USB first, so the root file system (root=) is mounted before
    // other drivers start: they may load firmware from /lib/firmware.
    dev::probe_all([](dev::Device* d) {
        u32 cls = d->class_code >> 8; // class + subclass
        return d->bus == dev::Bus::Pci && ((cls >> 8) == 0x01 || cls == 0x0C03);
    });
    xhci::enumerate_all();
    ehci::enumerate_all();
    uhci::enumerate_all();
    ohci::enumerate_all();
    // The log onto the boot stick from here on, before the other drivers
    // (GPU drivers) bind: if one blanks the screen, its log still lands there.
    bootlog::start();
    splash::stage(50, "Mounting file systems");
    mount_disk_root(); // root= on the command line: an installed system on disk
    dev::probe_all();
    sysfs::populate_devices();
    fbdev::init(); // /dev/fb0 (after the PCI devices are in /sys: fb0 may belong to one)

    // The EFI system partition (FAT32) appears at /boot/efi, as on Linux.
    // First pass: only the one Syluxium booted from (boot/syluxium.elf) -
    // not another system's EFI partition on the internal disk.
    for (int pass = 0; pass < 2; pass++)
    block::for_each([](block::Device* d, void* ctx) {
        bool ours_only = *static_cast<int*>(ctx) == 0;
        vfs::Inode* existing;
        if (vfs::resolve("/boot/efi", nullptr, false, &existing) == 0 && existing->mode != 0 &&
            vfs::is_dir(existing->mode) && existing->lookup("EFI", 3))
            return; // already mounted one
        if (!fat::probe(d))
            return;
        vfs::Inode* root;
        if (fat::mount(d, &root) != 0)
            return;
        vfs::Inode* efi_dir = root->lookup("EFI", 3);
        if (!efi_dir || !vfs::is_dir(efi_dir->mode))
            return; // a FAT volume, but not an EFI system partition (USB stick, SD card)
        if (ours_only) {
            vfs::Inode* boot_dir = root->lookup("boot", 4);
            if (!boot_dir || !vfs::is_dir(boot_dir->mode) || !boot_dir->lookup("syluxium.elf", 12))
                return;
        }
        vfs::mkdir_p("/boot/efi");
        char source[32];
        snprintf(source, sizeof(source), "/dev/%s", d->name);
        if (vfs::mount_at("/boot/efi", root, source, "vfat") == 0)
            klog::ok("Mounted %s (FAT32) on /boot/efi", source);
    }, &pass);

    // The first Linux (ext2/3/4) volume becomes /data: persistent storage.
    block::for_each([](block::Device* d, void* ctx) {
        auto* done = static_cast<bool*>(ctx);
        if (*done || d == g_root_dev || (g_root_dev && d == g_root_dev->parent) || !ext2::probe(d))
            return;
        if (matches_root_spec(d, "LABEL=SYLUXLIVE"))
            return; // a Syluxium stick's system partition, not storage
        vfs::Inode* root;
        bool ro;
        const char* kind;
        if (ext2::mount(d, &root, &ro, &kind) != 0)
            return;
        vfs::mkdir_p("/data");
        char source[32];
        snprintf(source, sizeof(source), "/dev/%s", d->name);
        if (vfs::mount_at("/data", root, source, kind) == 0) {
            klog::ok("Mounted %s on /data%s", source, ro ? " (read-only)" : "");
            *done = true;
        }
    }, &g_data_mounted);
    u32 counts[2] = {}; // [with driver, without]
    dev::for_each([](dev::Device* d, void* arg) {
        static_cast<u32*>(arg)[d->status == dev::Status::Ok ? 0 : 1]++;
    }, counts);
    klog::ok("Hardware: %u PCI functions, %u devices total (%u with drivers)", pci::count(), dev::count(), counts[0]);
    dev::for_each([](dev::Device* d, void*) {
        kprintf_serial("    %-14s %04x:%04x  %-22s %-10s %s\n", d->bus_id, d->vendor, d->device,
                       dev::category_name(d->category), d->driver_name ? d->driver_name : "-", d->name);
    }, nullptr);

    net::start(4000); // DHCP; wait a little so the shell starts online
}

void mount_initramfs() {
    vfs::mkdir_p("/tmp", 01777);
    vfs::mkdir_p("/run", 0755);
    vfs::mkdir_p("/dev/shm", 01777); // POSIX shared memory (shm_open) lives here, as on Linux
    auto* mods = boot::modules();
    if (!mods || mods->module_count == 0) {
        klog::warn("No initramfs module loaded: no user programs available");
        return;
    }
    limine_file* f = mods->modules[0];
    initramfs::Stats st;
    if (initramfs::unpack(f->address, f->size, &st) == 0)
        klog::ok("Initramfs: %u files, %u directories, %u symlinks (%lu KiB)", st.files, st.dirs, st.symlinks,
                 st.bytes / KiB);
}

// Run a program from the initramfs and wait for it. Returns the wait status.
int run_program(const char* const* argv) {
    static const char* const envp[] = {"HOME=/root", "PATH=/bin:/sbin:/usr/bin", "TERM=linux", "TZ=UTC",
                                       "USER=root", "SHELL=/bin/sh", nullptr};
    proc::Process* p;
    long r = proc::spawn(argv[0], argv, envp, &p);
    if (r < 0) {
        klog::error("Could not start %s (error %ld)", argv[0], -r);
        return -1;
    }
    return proc::wait(p);
}

const char* describe_status(int status, char* buf, usize len) {
    if (status < 0)
        snprintf(buf, len, "failed to start");
    else if ((status & 0x7F) != 0)
        snprintf(buf, len, "killed by signal %d", status & 0x7F);
    else
        snprintf(buf, len, "exit code %d", (status >> 8) & 0xFF);
    return buf;
}

void run_userspace_tests() {
    klog::info("Running static test programs (host gcc + glibc)...");
    u64 free_before = pmm::stats().free_pages;
    char desc[48];
    int failures = 0;

    struct Test {
        const char* const* argv;
        int expected; // wait status
    };
    static const char* const hello[] = {"/opt/syluxium/tests/hello", nullptr};
    static const char* const systest[] = {"/opt/syluxium/tests/systest", "arg-one", "arg two", nullptr};
    static const char* const crash[] = {"/opt/syluxium/tests/crashtest", nullptr};
    static const char* const threads[] = {"/opt/syluxium/tests/threadtest", nullptr};
    static const char* const nettest[] = {"/opt/syluxium/tests/nettest", nullptr};
    static const char* const unixtest[] = {"/opt/syluxium/tests/unixtest", nullptr};
    static const char* const nltest[] = {"/opt/syluxium/tests/nltest", nullptr};
    static const char* const ip6test[] = {"/opt/syluxium/tests/ip6test", nullptr};
    static const char* const eventtest[] = {"/opt/syluxium/tests/eventtest", nullptr};
    static const char* const shmtest[] = {"/opt/syluxium/tests/shmtest", nullptr};
    static const char* const inotifytest[] = {"/opt/syluxium/tests/inotifytest", nullptr};
    static const char* const ptytest[] = {"/opt/syluxium/tests/ptytest", nullptr};
    static const char* const credtest[] = {"/opt/syluxium/tests/credtest", nullptr};
    // 32-bit (ia32) programs: static glibc tests, then dynamic Debian i386 binaries.
    static const char* const ia32test[] = {"/opt/syluxium/tests/ia32test", nullptr};
    static const char* const ia32threads[] = {"/opt/syluxium/tests/ia32threads", nullptr};
    static const char* const ia32dynamic[] = {"/bin/sh", "/etc/syluxium/selftest-ia32.sh", nullptr};
    const Test sequential[] = {{hello, 0},       {systest, 0},     {crash, 11 /* SIGSEGV */}, {threads, 0},
                               {nettest, 0},     {unixtest, 0},    {nltest, 0},              {ip6test, 0},
                               {eventtest, 0},   {shmtest, 0},     {inotifytest, 0},          {ptytest, 0},     {credtest, 0},     {ia32test, 0},    {ia32threads, 0},          {ia32dynamic, 0}};

    for (const Test& t : sequential) {
        int status = run_program(t.argv);
        bool ok = status == t.expected;
        failures += !ok;
        (ok ? klog::ok : klog::error)("  %s: %s%s", t.argv[0], describe_status(status, desc, sizeof(desc)),
                                       ok ? (t.expected ? " (expected)" : "") : " - UNEXPECTED");
    }

    // Several FPU/AVX-heavy processes at once, spread over all CPUs: any bug
    // in saving/restoring register state between them corrupts their results.
    constexpr int STRESS = 6;
    proc::Process* procs[STRESS];
    static const char* const stress_argv[STRESS][3] = {
        {"fpustress", "1", nullptr}, {"fpustress", "2", nullptr}, {"fpustress", "3", nullptr},
        {"fpustress", "4", nullptr}, {"fpustress", "5", nullptr}, {"fpustress", "6", nullptr},
    };
    static const char* const envp[] = {"TZ=UTC", nullptr};
    int started = 0;
    for (int i = 0; i < STRESS; i++)
        if (proc::spawn("/opt/syluxium/tests/fpustress", stress_argv[i], envp, &procs[started]) == 0)
            started++;
    int stress_ok = 0;
    for (int i = 0; i < started; i++)
        stress_ok += proc::wait(procs[i]) == 0;
    bool stress_pass = stress_ok == STRESS;
    failures += !stress_pass;
    (stress_pass ? klog::ok : klog::error)("  fpustress x%d in parallel: %d/%d correct", STRESS, stress_ok, STRESS);

    sched::sleep_ms(50); // let the reaper free the exited threads
    i64 leaked = static_cast<i64>(free_before) - static_cast<i64>(pmm::stats().free_pages);
    if (failures == 0)
        klog::ok("All user programs behaved correctly (memory difference: %ld pages)", leaked);
    else
        klog::error("%d user program test(s) failed", failures);
}

void run_shell_tests() {
    klog::info("Running the shell self-test (Debian busybox sh, bash, GNU hello)...");
    static const char* const argv[] = {"/bin/sh", "/etc/syluxium/selftest.sh", nullptr};
    int status = run_program(argv);
    char desc[48];
    if (status == 0)
        klog::ok("Shell self-test passed");
    else
        klog::error("Shell self-test: %s", describe_status(status, desc, sizeof(desc)));
}

// An interactive login shell on the console, restarted whenever it exits.
void console_shell_loop() {
    static const char* const argv[] = {"-bash", nullptr};
    static const char* const envp[] = {"HOME=/root", "PATH=/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin",
                                       "TERM=linux", "TZ=UTC", "USER=root", "LOGNAME=root", "SHELL=/bin/bash",
                                       nullptr};
    for (;;) {
        vfs::Inode* motd;
        char buf[512];
        if (vfs::resolve("/etc/motd", nullptr, true, &motd) == 0) {
            isize n = motd->read(0, buf, sizeof(buf));
            if (n > 0)
                kwrite(buf, static_cast<usize>(n));
        }
        proc::Process* p;
        if (proc::spawn("/bin/bash", argv, envp, &p) < 0) {
            klog::error("Could not start /bin/bash");
            return;
        }
        tty::set_foreground_group(p->pgid);
        proc::wait(p);
    }
}

// Second half of boot, running as the first real kernel thread.
void kernel_main_thread(void*) {
    splash::start_animation(); // the first thread: the load bar moves from here on
    u32 cpus = smp::start_aps();
    klog::ok("SMP: %u CPU(s) online, scheduler running on all of them", cpus);

    // Every CPU is now off the bootloader's stacks and page tables.
    sysfs::init(); // needs the final CPU count
    cpupower::publish(); // cpufreq files; idle CPUs go tickless from here
    watchdog::start();   // stuck CPUs print where they are
    splash::stage(30, "Starting drivers");
    discover_hardware();
    splash::stage(62, "Starting services");
    acpi::start_interpreter(); // AML: power button, battery, lid (after PCI)
    linuxkpi::acpi_ready();    // GPU probes that needed ACPI methods (_ROM)
    ps2kbd::late_init(); // after the USB handoff (firmware PS/2 emulation off)
    u64 reclaimed = pmm::reclaim_bootloader_memory();
    klog::ok("Reclaimed %lu KiB of bootloader memory", reclaimed / KiB);

    splash::stage(72, "Checking the system");
    selftest::run_scheduler_tests();
    selftest::run_fpu_tests();
    run_userspace_tests();
    run_shell_tests();

    auto ps = pmm::stats();
    klog::info("Memory: %lu MiB free of %lu MiB; %lu kernel threads",
               ps.free_pages * 4 / 1024, ps.total_pages * 4 / 1024, sched::thread_count());
    for (u32 i = 0; i < percpu::count(); i++) {
        Cpu* c = percpu::get(i);
        kprintf("    CPU %u (LAPIC %u): %lu timer ticks, %lu context switches\n",
                c->index, c->lapic_id, c->ticks, c->context_switches);
    }
    klog::ok("Phase 4b complete. Starting an interactive shell on the console.");
    // "splashhold" (tests): keep the splash like a desktop boot does, until Esc.
    splash::boot_finished(strstr(boot::cmdline(), "desktop") != nullptr || strstr(boot::cmdline(), "splashhold") != nullptr);
    console_shell_loop();
}

} // namespace

extern "C" [[noreturn]] void kmain() {
    serial::init();
    percpu::init_bsp(); // GS -> boot CPU's Cpu: required before any lock is taken

    if (!boot::base_revision_supported()) {
        serial::write("syluxium: bootloader too old (Limine base revision 6 required)\n", 63);
        arch::halt_forever();
    }

    cxx::run_global_constructors();
    init_display();
    print_banner();

    if (!serial::present())
        klog::warn("no serial port found, logging to screen only");
    if (!console::ready())
        klog::warn("no usable framebuffer, logging to serial only");

    gdt::init_bsp(this_cpu()->tables);
    klog::ok("GDT and TSS loaded");

    interrupts::init();
    klog::ok("IDT loaded (256 vectors)");

    pic::disable();
    klog::ok("Legacy PIC remapped and masked");

    // Self-test: int3 goes through the whole interrupt path and returns here.
    asm volatile("int3");
    klog::ok("Interrupt round-trip works");

    print_cpu_info();
    if (auto* ka = boot::kernel_address())
        klog::info("Kernel loaded at phys %#lx, virt %#lx", ka->physical_base, ka->virtual_base);
    print_memory_map();

    // ---- Phase 2: memory management ---------------------------------------
    pmm::init();
    auto ps = pmm::stats();
    klog::ok("Physical memory manager: %lu MiB managed, page database %lu KiB",
             ps.total_pages * 4 / 1024, ps.database_bytes / KiB);

    vmm::init();
    klog::ok("Kernel page tables active (NX %s, 1 GiB pages %s, %lu table pages)",
             vmm::nx_supported() ? "on" : "unsupported",
             vmm::huge_pages_supported() ? "on" : "unsupported", vmm::page_table_pages());

    selftest::run_memory_tests();

    fpu::init_bsp();
    syscall::init_cpu();
    syscall::register_all();
    klog::ok("User mode support: %s state, SYSCALL entry, Linux syscall table", fpu::mode_name());

#ifdef SYLUX_TEST_GUARD_FAULT
    // Deliberate crash: write into a vmalloc guard page. Must end in a clean
    // page-fault panic, proving faults are caught under our own page tables.
    auto* victim = static_cast<volatile u8*>(vmalloc::alloc(4096));
    klog::warn("SYLUX_TEST_GUARD_FAULT: writing below vmalloc block %p", const_cast<u8*>(victim));
    victim[-1] = 1;
#endif

    // ---- Phase 3: hardware discovery, timers, scheduler -------------------
    acpi::init();
    acpi::power_init();
    const auto& madt = acpi::madt();
    klog::ok("ACPI %s: %u CPU(s), %u I/O APIC(s), %u interrupt override(s)%s",
             acpi::revision() >= 2 ? "2.0+" : "1.0", madt.cpu_count, madt.ioapic_count,
             madt.override_count, acpi::hpet().present ? ", HPET" : "");

    apic::init_bsp();
    ioapic::init();
    klog::ok("Local APIC enabled (%s mode, id %u); %u I/O APIC(s) masked",
             apic::x2apic_mode() ? "x2APIC" : "xAPIC", apic::local_id(), ioapic::count());

    clock::init();
    klog::ok("Clock: %s, TSC at %lu MHz", clock::source_name(), clock::tsc_frequency_hz() / 1'000'000);
    random::init();
    rtc::init();

    apic::calibrate_timer();
    klog::ok("APIC timer: %u ticks/ms, scheduler tick %u Hz", apic::timer_ticks_per_ms(), sched::TICK_HZ);

    // ---- Phase 4: files, devices, processes --------------------------------
    vfs::init();
    tty::init();
    input::init();
    g_keyboard = ps2kbd::init();
    devices::init();
    proc::init();
    mount_initramfs(); // before bootloader memory is reclaimed (the module list lives there)
    procfs::init();
    vfs::mkdir_p("/mnt");   // mount points Debian's base layout no longer ships
    vfs::mkdir_p("/media");
    klog::ok("Files, devices and processes ready (keyboard: %s)", g_keyboard ? "PS/2" : "none, serial only");

    sched::init();
    sched::spawn("kmain", kernel_main_thread, nullptr);
    cpupower::init_cpu();
    apic::start_timer(sched::TICK_HZ);
    sched::enter(); // never returns: the boot stack is abandoned here
}
