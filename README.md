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
- [x] **Phase 4c**: threads (clone/futex, per-thread signals, TLB shootdown per address space),
      /proc (processes, CPU/memory accounting, cpuinfo, meminfo, stat, ...), /sys (CPUs)
- [ ] **Phase 4d** (before phase 10): 32-bit (ia32) program support, needed by Steam
- [ ] **Phase 5**: PCI, AHCI/NVMe, USB (xHCI) keyboard/mouse/storage, driver model with a
      device registry (every device: name, vendor, driver, status, resources), ext4 so files persist
      - [x] 5a: PCI + device registry + driver model, block layer + cache + partitions, AHCI, NVMe,
            FAT32 (rw), ext2 (rw) / ext3 / ext4 (ro), mount/umount/rename/statfs/utimensat
      - [x] 5b: PS/2 mouse (wheel), Linux input events (/dev/input/eventN, evdev ioctls), USB:
            xHCI host controller, hubs, HID keyboards/mice, mass storage (USB sticks as /dev/sdX),
            kernel log buffer (dmesg)
      - [x] 5c: exFAT read/write (long names, rename; fsck.exfat-clean), mount type auto-detection
      - [ ] later: USB hot-plug, HID report parsing
            (gamepads, tablets, media keys), USB 3 hubs
- [ ] **Phase 6**: wired networking: virtio-net/e1000/Realtek, Ethernet/ARP/IPv4/IPv6/UDP/TCP, DHCP, DNS
      - [x] 6a: Intel e1000/e1000e (82540EM, 82574L, I217-I219) and virtio-net drivers; Ethernet,
            ARP, IPv4, ICMP (ping), UDP, TCP (retransmission, RTT-based timeout, flow control,
            listen/accept, half-close), loopback, kernel DHCP client, /etc/resolv.conf, BSD socket
            API (glibc DNS, wget, ping, httpd, nc work), ifconfig/route ioctls, /proc/net/*,
            alarm/setitimer
      - [ ] 6b: Realtek r8169 (most desktop boards), IPv6, AF_UNIX sockets, netlink (ip command),
            TCP window scaling/SACK, IP fragment reassembly
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
      - **Disk Management** (partitions, format, mount, drive letters/labels) and **Computer
        Management** (one console: Device Manager, Disk Management, services, event log, users)
      - Accessories (original Syluxium apps, Windows-style): **File Explorer**, **Notepad**,
        **WordPad** (rich text), **Paint**, **Calculator**, **Character Map**, **Snipping Tool**
        (screenshots + markup), **Media Player** (music and video)
      - Accessibility: **Magnifier**, **On-Screen Keyboard**
      - Network apps: **Network and Sharing Center** (connections, adapters, sharing),
        **Firewall** (inbound/outbound rules), **Remote Desktop** client (RDP/VNC)
      - **Printing**: print services (CUPS-compatible spooler, printer drivers, Devices and Printers)
      - **Programming tools** (GUI): an IDE like KDevelop (projects, editor with highlighting and
        completion, build, debugger front end for gdb), plus GUI tools around compilers (gcc/clang,
        Python, ...); real IDEs (KDevelop, Qt Creator, VS Code-likes, Geany) installable from the App Store
      - **Image Writer** (GUI): flash .iso/.img (and compressed images) to USB sticks and SD cards
        with verification, like balenaEtcher / Rufus
      - Every system tool has a GUI (command line optional)
- [ ] **Phase 10**: GPU drivers and acceleration, for gaming and other graphics work: virtio-gpu
      (virgl) first, then AMD (amdgpu) and NVIDIA (nouveau, and NVIDIA's open kernel modules)
      through the driver compatibility layer. Display side (KMS): resolutions, refresh rates,
      multiple monitors. 3D: Mesa's **Vulkan** (RADV, NVK) and **OpenGL** (radeonsi, zink/nouveau)
      drivers, so Steam, Proton and other games and 3D apps run
- [ ] **Phase 11**: **App Store** + package installer (.deb only; .rpm dropped): a Play Store-like
      app with screenshots, icons, descriptions, ratings/details, categories and search, pulling
      .deb packages from several repositories (Debian, Ubuntu, third-party apt repos); double-click
      a .deb to install it too; dependencies resolved by apt; apps appear in the start menu

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
- ✅ User-space threads + futexes
- ⬜ Shared memory, Unix domain sockets
- ✅ /proc (process list, per-process CPU/memory, cpuinfo, meminfo, stat) and /sys (CPUs;
     devices come with the phase 5 driver model)
- ⬜ 32-bit (ia32) program support
- 🟡 High-resolution timers: ns clock done; precise timer interrupts to do

**Drivers**
- ✅ Serial, PS/2 keyboard, firmware framebuffer
- ✅ PCI/PCIe enumeration (ECAM, BARs, MSI/MSI-X) + device registry (/sys/bus/pci, lspci works,
     /sys/kernel/syluxium/devices for the Device Manager)
- ✅ PS/2 mouse (with wheel)
- 🟡 USB (xHCI + hubs): keyboards, mice, storage work; gamepads (HID report parsing) and hot-plug to do
- ✅ Input event interface (/dev/input/eventN, Linux evdev ABI) for games and the desktop
- ✅ AHCI/SATA, NVMe (polled DMA; interrupts/NCQ later)
- ⬜ Wired network cards
- ⬜ Linux driver compatibility layer (shared by Wi-Fi and GPU)
- ⬜ Wi-Fi: Intel, Qualcomm, MediaTek, Realtek, Broadcom
- ⬜ Audio: Intel HDA, USB audio, ALSA-compatible interface
- ⬜ GPU: AMD + NVIDIA kernel drivers, DRM/KMS (resolution, refresh, multi-monitor, vsync,
     Vulkan/OpenGL via Mesa)

**Storage**
- ✅ VFS, RAM filesystem, initramfs
- ✅ ext2/3/4: reads all three (extents, 64-bit); writes ext2-style volumes (e2fsck-clean).
     ext4 is read-only by design (other systems' Linux partitions stay safe); a journal for
     Syluxium's own volumes (crash safety) is planned before installing on real PCs
- ✅ FAT32 and exFAT read/write with long file names (ESP mounted at /boot/efi)
- ✅ Disk cache (4 KiB blocks, write-back every 5 s and on sync), MBR + GPT partitions

**Networking**
- 🟡 TCP/IP stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP done (ping google.com, wget, inbound
     HTTP verified); IPv6 to do. Drivers: e1000/e1000e, virtio-net; Realtek r8169 to do
- 🟡 Sockets API: AF_INET (TCP, UDP, ICMP) done; AF_UNIX (needed by the desktop) to do
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
- ⬜ Disk Management, Computer Management
- ⬜ File Explorer, Notepad, WordPad, Paint, Calculator, Character Map, Snipping Tool, Media Player
- ⬜ Magnifier, On-Screen Keyboard
- ⬜ Network and Sharing Center, Firewall (kernel packet filter + rules UI), Remote Desktop client
- ⬜ Printing (CUPS-compatible print services, Devices and Printers)
- ⬜ Programming tools: KDevelop-like IDE (editor, build, gdb debugger front end)
- ⬜ Image Writer: flash .iso/.img to USB/SD with verification
- ⬜ Notifications

**Packages and system**
- 🟡 Package installer: build-time Debian fetcher done; App Store + double-click .deb on the device to do
- ⬜ Running packages' maintainer (install) scripts
- ⬜ Users and permissions (everything runs as root today)
- 🟡 Security: NX, kernel W^X, guard pages done; ASLR, SMEP/SMAP to do
- ⬜ Power management: sleep, battery, CPU frequency scaling
- ⬜ Updates and recovery

Tally: 21 done · 9 partly done · 34 to do.
