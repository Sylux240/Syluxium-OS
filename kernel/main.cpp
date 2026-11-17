#include <acpi/acpi.hpp>
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

// Phase 5: find the hardware, bind drivers, publish it all in /sys.
void discover_hardware() {
    block::init();
    ahci::register_driver();
    nvme::register_driver();
    xhci::register_driver();
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
    dev::probe_all();
    xhci::enumerate_all();
    sysfs::populate_devices();

    // The EFI system partition (FAT32) appears at /boot/efi, as on Linux.
    block::for_each([](block::Device* d, void*) {
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
        vfs::mkdir_p("/boot/efi");
        char source[32];
        snprintf(source, sizeof(source), "/dev/%s", d->name);
        if (vfs::mount_at("/boot/efi", root, source, "vfat") == 0)
            klog::ok("Mounted %s (FAT32) on /boot/efi", source);
    }, nullptr);

    // The first Linux (ext2/3/4) volume becomes /data: persistent storage.
    block::for_each([](block::Device* d, void* ctx) {
        auto* done = static_cast<bool*>(ctx);
        if (*done || !ext2::probe(d))
            return;
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
    vfs::mkdir_p("/tmp", 01777);
    vfs::mkdir_p("/dev/shm", 01777); // POSIX shared memory (shm_open) lives here, as on Linux
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
    // 32-bit (ia32) programs: static glibc tests, then dynamic Debian i386 binaries.
    static const char* const ia32test[] = {"/opt/syluxium/tests/ia32test", nullptr};
    static const char* const ia32threads[] = {"/opt/syluxium/tests/ia32threads", nullptr};
    static const char* const ia32dynamic[] = {"/bin/sh", "/etc/syluxium/selftest-ia32.sh", nullptr};
    const Test sequential[] = {{hello, 0},       {systest, 0},     {crash, 11 /* SIGSEGV */}, {threads, 0},
                               {nettest, 0},     {unixtest, 0},    {nltest, 0},              {ip6test, 0},
                               {eventtest, 0},   {shmtest, 0},     {ia32test, 0},    {ia32threads, 0},          {ia32dynamic, 0}};

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
    u32 cpus = smp::start_aps();
    klog::ok("SMP: %u CPU(s) online, scheduler running on all of them", cpus);

    // Every CPU is now off the bootloader's stacks and page tables.
    sysfs::init(); // needs the final CPU count
    discover_hardware();
    u64 reclaimed = pmm::reclaim_bootloader_memory();
    klog::ok("Reclaimed %lu KiB of bootloader memory", reclaimed / KiB);

    selftest::run_scheduler_tests();
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
    apic::start_timer(sched::TICK_HZ);
    sched::enter(); // never returns: the boot stack is abandoned here
}
