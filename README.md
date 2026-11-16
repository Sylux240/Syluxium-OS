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
- [ ] **Phase 4c**: threads (clone/futex), 32-bit compat syscalls, /proc (process list,
      CPU/memory usage per process), /sys
- [ ] **Phase 5**: PCI, AHCI/NVMe, USB (xHCI) keyboard/mouse/storage, driver model with a
      device registry (every device: name, vendor, driver, status, resources), ext4 so files persist
- [ ] **Phase 6**: wired networking: virtio-net/e1000/Realtek, Ethernet/ARP/IPv4/IPv6/UDP/TCP, DHCP, DNS
- [ ] **Phase 7**: Linux driver compatibility layer (LinuxKPI) + **Wi-Fi** for many chips: Intel
      (iwlwifi), Qualcomm/Atheros (ath9k/10k/11k/12k), MediaTek (mt76), Realtek (rtw88/rtw89),
      Broadcom (brcmfmac); mac80211/cfg80211, WPA2/WPA3 via wpa_supplicant/iwd, linux-firmware
- [ ] **Phase 8**: **audio**: ALSA-compatible kernel interface (so libasound, PipeWire and
      PulseAudio apps work), drivers for Intel HD Audio, USB audio, virtio-sound; volume control
- [ ] **Phase 9**: Wayland compositor, GUI toolkit, Windows-style desktop (taskbar, start menu).
      Look: clean, glossy and rounded like Windows XP / Windows 7 (not Windows 11).
      - Right-click context menus everywhere: on files (Open, Open with, Extract Here / Extract
        to..., Compress, Rename, Delete, Properties, Change icon), on folders and empty space
        (New > Folder / Text document / ..., View, Sort by, Paste), on the desktop (Display
        settings, Personalize)
      - File manager with archive extraction (zip, tar.gz/xz/zst, 7z, rar)
      - Display settings: screen resolution, refresh rate, scaling, multiple monitors
      - Personalize: themes, colors, wallpapers, icons, cursor, sounds
      - System apps:
      - Control Panel / Settings with all the usual categories
      - **Task Manager**: running programs and processes, CPU/memory/disk/network use, End Task
      - **System page** (like Windows "About" / macOS "About This Mac"): OS version, CPU, RAM,
        GPU, storage, uptime
      - **Device Manager**: tree of all hardware by category, driver and status per device
- [ ] **Phase 10**: GPU drivers and acceleration, for gaming and other graphics work: virtio-gpu
      (virgl) first, then AMD (amdgpu) and NVIDIA (nouveau, and NVIDIA's open kernel modules)
      through the driver compatibility layer. Display side (KMS): resolutions, refresh rates,
      multiple monitors. 3D: Mesa's **Vulkan** (RADV, NVK) and **OpenGL** (radeonsi, zink/nouveau)
      drivers, so Steam, Proton and other games and 3D apps run
- [ ] **Phase 11**: package installer: double-click a .deb or .rpm to install it (dependencies
      fetched from Debian / Fedora repos into per-distro runtimes), apps appear in the start menu

## Checklist

What Syluxium itself has to build. Things Debian already provides and that run
unmodified once the kernel supports them are deliberately left out: glibc,
OpenSSL/GnuTLS, FreeType, GTK/Qt, FFmpeg, PipeWire, Mesa's user-space Vulkan/OpenGL
libraries, browsers, Wine, DHCP/DNS clients, the shell and core utilities.

Legend: ✅ done · 🟡 partly done · ⬜ to do

**Boot**
- ✅ Bootloader (Limine, BIOS + UEFI), 64-bit long mode, GDT/IDT, memory map
- 🟡 ACPI: tables parsed; AML interpreter (port uACPI) for shutdown/sleep/battery still to do

**Kernel core**
- ✅ Memory management, interrupts, APIC timer, preemptive SMP scheduler
- ✅ Processes, user/kernel split, Linux system calls (core set)
- ✅ Pipes, signals
- ⬜ User-space threads + futexes
- ⬜ Shared memory, Unix domain sockets
- ⬜ /proc and /sys
- ⬜ 32-bit (ia32) program support
- 🟡 High-resolution timers: ns clock done; precise timer interrupts to do

**Drivers**
- ✅ Serial, PS/2 keyboard, firmware framebuffer
- ⬜ PCI/PCIe enumeration + device registry (for Device Manager)
- ⬜ PS/2 mouse
- ⬜ USB (xHCI): keyboards, mice, gamepads, storage
- ⬜ Input event interface (/dev/input): raw mouse + gamepads for games and the desktop
- ⬜ AHCI/SATA, NVMe
- ⬜ Wired network cards
- ⬜ Linux driver compatibility layer (shared by Wi-Fi and GPU)
- ⬜ Wi-Fi: Intel, Qualcomm, MediaTek, Realtek, Broadcom
- ⬜ Audio: Intel HDA, USB audio, ALSA-compatible interface
- ⬜ GPU: AMD + NVIDIA kernel drivers, DRM/KMS (resolution, refresh, multi-monitor, vsync,
     Vulkan/OpenGL via Mesa)

**Storage**
- ✅ VFS, RAM filesystem, initramfs
- ⬜ ext4 (installs survive reboot)
- ⬜ FAT32 / exFAT (USB sticks, sharing with Windows)
- ⬜ Disk cache

**Networking**
- ⬜ TCP/IP stack: Ethernet, ARP, IPv4/IPv6, UDP, TCP
- ⬜ Sockets API
- ⬜ Netlink (network tools, Wi-Fi configuration)

**Linux compatibility**
- ✅ ELF loader, dynamic linking, Debian userland running unmodified
- 🟡 System-call coverage (grows with every phase)

**Desktop** (Syluxium's own; XP/7 style: clean, rounded, glossy)
- ⬜ Wayland compositor + desktop shell: taskbar, start menu
- ⬜ Right-click menus: extract, compress, new file/folder, change icon, properties
- ⬜ File manager with archive extraction
- ⬜ Display settings: resolution, refresh rate, scaling, multi-monitor
- ⬜ Personalize: themes, colors, wallpapers, icons, cursors
- ⬜ Control Panel / Settings
- ⬜ Task Manager with End Task
- ⬜ System page: OS version, CPU, RAM, GPU, storage
- ⬜ Device Manager
- ⬜ Notifications

**Packages and system**
- 🟡 Package installer: build-time Debian fetcher done; double-click .deb/.rpm on the device to do
- ⬜ Running packages' maintainer (install) scripts
- ⬜ Users and permissions (everything runs as root today)
- 🟡 Security: NX, kernel W^X, guard pages done; ASLR, SMEP/SMAP to do
- ⬜ Power management: sleep, battery, CPU frequency scaling
- ⬜ Updates and recovery

Tally: 12 done · 6 partly done · 39 to do.
