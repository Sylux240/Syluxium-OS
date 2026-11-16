# Syluxium

A from-scratch x86_64 operating system written in C++20: monolithic kernel,
booted by [Limine](https://github.com/limine-bootloader/limine).

## Requirements (Arch / CachyOS)

```sh
sudo pacman -S clang lld make git python mtools util-linux cpio gcc qemu-full
sudo pacman -S edk2-ovmf   # optional, for `make run-uefi`
```

## Build & run

```sh
make            # build/syluxium.elf + bootable disk image build/syluxium.img
make run        # boot in QEMU (BIOS); kernel log also appears in this terminal
make run-uefi   # boot in QEMU with UEFI firmware
make debug      # QEMU waits for GDB:  gdb build/syluxium.elf -ex 'target remote :1234'
```

After the boot self-tests you get an interactive Debian **bash** shell: type in the
QEMU window (PS/2 keyboard) or in the terminal (serial). The userland is built
from official Debian packages (`DEBIAN_PKGS` in the Makefile).

Headless test: `tools/smoke-test.sh [bios|uefi] [seconds] [label]` boots without
a window and captures the serial log + a screenshot. With a label, both are
archived to `smoke/` (one screenshot per major test; survives `make clean`).

The first build clones Limine into `deps/`. `make` also writes
`compile_commands.json`, so CLion / clangd get full code completion
(in CLion: *File → Open* the `compile_commands.json` as a project).

To boot real hardware, write the image to a USB stick
(`sudo dd if=build/syluxium.img of=/dev/sdX bs=4M`). This erases the stick.

## Layout

```
kernel/
  main.cpp            kmain: boot sequence
  linker.ld           higher-half kernel layout
  arch/x86_64/        CPU: GDT/TSS, IDT + ISR stubs, PIC, port I/O, MSRs
  boot/               Limine requests (memory map, framebuffer, HHDM, ...)
  core/               logging (kprintf/klog), panic
  cxx/                freestanding C++ runtime support
  drivers/            device drivers (serial so far)
  lib/                string functions, printf engine, spinlocks
  acpi/               ACPI table parsing (MADT, HPET, ...)
  fs/                 VFS, ramfs, /dev devices, initramfs (cpio) unpacker
  mm/                 memory: pmm (buddy), vmm (page tables), heap (slab), vmalloc
  proc/               processes, user memory (VMAs, demand paging), ELF loader
  sched/              SMP scheduler, threads, mutex/semaphore/completion/wait queues
  syscall/            Linux x86_64 system call layer
  tests/              boot-time self-tests
  time/               clock (TSC/HPET), RTC wall clock
  video/              framebuffer, text console, font
userspace/             test programs: built with the host gcc + glibc (static), i.e. real Linux binaries
tools/debian-fetch.py  downloads official Debian packages (+ dependencies, SHA256-checked) into the rootfs
rootfs/               files copied into the initramfs (/etc, ...)
tools/psf2cpp.py      converts a PSF console font into kernel/video/font_*.cpp
tools/smoke-test.sh   headless QEMU boot: serial log + screenshot
```

## Roadmap

- [x] **Phase 1**: boot, serial + framebuffer console, GDT/TSS, IDT, exceptions
- [x] **Phase 2**: buddy page allocator, kernel page tables (W^X, NX, PAT), slab heap, vmalloc
- [x] **Phase 3**: ACPI, local APIC + I/O APIC, timers, SMP, scheduler, threads
- [x] **Phase 4a**: user mode, Linux syscall ABI (core set), static ELF loader, VFS, ramfs, initramfs, /dev, tty
- [x] **Phase 4b**: fork/exec/wait (copy-on-write), pipes, signals, poll/select, dynamic linker,
      PS/2 keyboard, VT100 console, interactive Debian bash shell
- [ ] **Phase 4c**: threads (clone/futex), 32-bit compat syscalls, /proc, /sys
- [ ] **Phase 5**: PCI, AHCI/NVMe, PS/2 + USB (xHCI) input, driver model
- [ ] **Phase 6**: networking: virtio-net/e1000, Ethernet/ARP/IPv4/UDP/TCP, DHCP, DNS
- [ ] **Phase 7**: Wayland compositor, GUI toolkit, Windows-style desktop (taskbar, start menu, Control Panel)
- [ ] **Phase 8**: GPU acceleration: virtio-gpu (virgl), Mesa port
- [ ] **Phase 9**: package installer: double-click a .deb or .rpm to install it (dependencies
      fetched from Debian / Fedora repos into per-distro runtimes), apps appear in the start menu
