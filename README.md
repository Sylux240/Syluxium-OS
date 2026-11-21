# Syluxium

A from-scratch x86_64 operating system written in C,C++20, and Python: monolithic kernel, (Aided with AI for more efficient workflows and productivity)
booted by [Limine](https://github.com/limine-bootloader/limine).

## Screenshots

From power-on to the desktop, in the order a computer shows them:

| | |
|---|---|
| **1. Boot picker** (UEFI): pick a system with the mouse or keyboard | **2. Boot splash**: logo and load bar while Syluxium starts |
| ![Boot picker](docs/screenshots/1-boot-picker.png) | ![Boot splash](docs/screenshots/2-boot-splash.png) |
| **3. Verbose boot** (press Esc during the splash) | **4. Login screen** |
| ![Verbose boot](docs/screenshots/3-verbose-boot.png) | ![Login screen](docs/screenshots/4-login.png) |
| **5. Desktop** | |
| ![Desktop](docs/screenshots/5-desktop.png) | |

## Made On Linux (Arch / CachyOS) 

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
archived to `smoke/` (one screenshot per major test; survives `make clean`; kept
locally, not in the repository).

The first build clones Limine into `deps/`. `make` also writes
`compile_commands.json`, so CLion / clangd get full code completion
(in CLion: *File → Open* the `compile_commands.json` as a project).

To boot real hardware, write the image to a USB stick
(`sudo dd if=build/syluxium.img of=/dev/sdX bs=4M`). This erases the stick.

Booting: on UEFI machines a graphical **boot picker** comes first (large icons for Syluxium,
Text Console, Safe Mode and any Windows/macOS/Linux found on the disks; arrow keys, Enter,
mouse; Esc = Limine's text menu), then the **boot splash** (logo + load bar) until the login
screen. Press **Esc** during the splash for the verbose boot log. Boot options (Limine entry,
`cmdline:`): `verbose` (no splash), `nodrm` / `xfbdev` (old framebuffer display path), `nogpu`
(no GPU drivers), `noaccel` (GPU driver without acceleration), `acpi=noaml nohz=off idle=hlt`
(safe mode).

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
  video/              framebuffer, text console, font, boot splash, /dev/fb0
  linuxkpi/           Linux driver compatibility layer + vendored Linux 6.12 drivers (Wi-Fi, DRM, ...)
boot/picker/          graphical UEFI boot picker (runs before Limine)
userspace/             test programs: built with the host gcc + glibc (static), i.e. real Linux binaries
tools/debian-fetch.py  downloads official Debian packages (+ dependencies, SHA256-checked) into the rootfs
rootfs/               files copied into the initramfs (/etc, ...)
tools/psf2cpp.py      converts a PSF console font into kernel/video/font_*.cpp
tools/smoke-test.sh   headless QEMU boot: serial log + screenshot
tools/wine-setup.sh   finishes Wine in the rootfs: WoW64 DLLs, stripping, template prefix (+ wine-prefix-links.py)
userspace/windows/    Windows test programs (clang + lld-link) for Wine
```

## Checklist

What Syluxium itself has to build. Things Debian already provides and that run
unmodified once the kernel supports them are deliberately left out: glibc,
OpenSSL/GnuTLS, FreeType, GTK/Qt, FFmpeg, PipeWire, Mesa's user-space Vulkan/OpenGL
libraries, browsers, Wine, DHCP/DNS clients, the shell and core utilities.

Legend: ✅ done · 🟡 partly done · ⬜ to do

**Boot**
- ✅ Bootloader (Limine, BIOS + UEFI), 64-bit long mode, GDT/IDT, memory map
- ✅ Graphical UEFI boot picker (icons, mouse + keyboard, other systems detected) and boot splash
     (logo + load bar; Esc = live verbose log)
- ✅ ACPI: tables + AML interpreter (uACPI): power off / restart, power button, battery, AC adapter, lid

**Kernel core**
- ✅ Memory management, interrupts, APIC timer, preemptive SMP scheduler
- ✅ Processes, user/kernel split, Linux system calls (core set)
- ✅ Pipes, signals
- ✅ User-space threads + futexes
- ✅ Unix domain sockets, shared memory (memfd + seals, POSIX shm, System V shm/semaphores, MAP_SHARED)
- ✅ /proc (process list, per-process CPU/memory, cpuinfo, meminfo, stat) and /sys (CPUs, PCI
     devices, power supplies)
- ✅ 32-bit (ia32) program support
- ✅ Timers: nanosecond clock, one-shot APIC timer, tickless idle
- ✅ Users and permissions: real/effective user and group IDs, file permissions, sudo

**Drivers**
- ✅ Serial, PS/2 keyboard and mouse (with wheel), firmware framebuffer
- ✅ PCI/PCIe enumeration (ECAM, BARs, MSI/MSI-X) + device registry (/sys/bus/pci, lspci works,
     /sys/kernel/syluxium/devices for the Device Manager)
- ✅ USB: xHCI, EHCI, UHCI, USB 2/3 hubs, plug and play (unplugging never crashes): keyboards,
     mice, tablets, gamepads, Xbox pads, storage, audio, Ethernet, Wi-Fi
- ✅ Input event interface (/dev/input/eventN, Linux evdev ABI) for games and the desktop
- 🟡 Touchpads: Apple USB trackpads (bcm5974) work; I2C-HID / Synaptics / Elantech precision touchpads to do
- ✅ AHCI/SATA, NVMe (polled DMA; interrupts/NCQ later)
- ✅ Wired network cards: Intel e1000/e1000e, virtio-net, RTL8139 (Linux 8139cp), USB Ethernet
     (CDC-ECM), Realtek r8169 (untested)
- 🟡 Linux driver compatibility layer (LinuxKPI): core, PCI, networking, USB, wireless (cfg80211/mac80211), DRM core and page model done; GPU drivers in progress
- 🟡 Wi-Fi: Realtek USB (rtl8xxxu) works with WPA2 and plug and play; MediaTek USB (mt7601u) built in; Intel, Qualcomm, Broadcom, PCIe Realtek/MediaTek to do
- ✅ Audio: ALSA-compatible interface, Intel HDA, virtio-sound, USB Audio (works on real laptops)
- 🟡 GPU: DRM/KMS core + simpledrm + Xorg modesetting done; Intel i915 builds (hardware bring-up next);
     AMD + NVIDIA drivers, multi-monitor, vsync, Vulkan/OpenGL via Mesa to do

**Storage**
- ✅ VFS, RAM filesystem, initramfs
- ✅ ext4 read-write with a jbd2 journal (crash safety); ext2/3 too
- ✅ FAT32 and exFAT read/write with long file names (ESP mounted at /boot/efi)
- ✅ Disk cache (4 KiB blocks, write-back every 5 s and on sync), MBR + GPT partitions
- ✅ USB drives mounted automatically, AutoPlay bubble (open folder / play media)

**Networking**
- ✅ TCP/IP stack: Ethernet, ARP, IPv4 + IPv6, ICMP, UDP, TCP (window scaling), DHCP, SLAAC,
     fragments. Drivers: e1000/e1000e, virtio-net, RTL8139 (Linux 8139cp), r8169 (untested)
- ✅ Sockets API: AF_INET/AF_INET6, AF_UNIX (fd passing), AF_NETLINK, AF_PACKET; epoll, eventfd,
     timerfd, signalfd, inotify
- ✅ Netlink: rtnetlink (ip, getifaddrs), generic netlink with nl80211 (Wi-Fi)

**Linux compatibility**
- ✅ ELF loader, dynamic linking, Debian userland running unmodified
- 🟡 System-call coverage (grows with every phase)

**Desktop** (Syluxium's own; XP/7 style: clean, rounded, glossy)
- ✅ Desktop: Trinity (TDE) on Xorg with an XP-era look (original artwork), green "Start" button,
     welcome-screen login, Windows keys (Win, Win+E/R/D/L, Ctrl+Alt+Del); desktop chooser later
- 🟡 Right-click menus: file manager and desktop menus work; extract/compress, change icon to do
- 🟡 File manager: Konqueror as My Computer / My Documents with the Address bar; archive extraction to do
- 🟡 Display settings: resolution (applied at the next boot); live changes, refresh rate, multi-monitor
     need the GPU drivers
- 🟡 Personalize: themes, colors, wallpapers, icons (TDE Appearance & Themes); XP restyling of the dialogs to do
- 🟡 Control Panel: TDE Control Center with XP category names; full restyle to do
- ✅ Task Manager (Ctrl+Alt+Del / Ctrl+Shift+Esc): Applications (End Task, Switch To), Processes
     (End Process, priority), Performance and Networking graphs, XP style
- ✅ System page: Syluxium version, processor, memory, graphics, storage
- ✅ Device Manager: every device by category, driver and status, problem devices flagged
- ✅ Computer Management (Event Viewer, Services, Local Users, Device Manager, Disk Management)
     and Disk Management with partitioning and formatting (GParted)
- 🟡 Accessories: File Explorer (Konqueror), Media Player (Kaffeine), E-mail (KMail), Windows
     Notepad (Wine); WordPad, Paint, Calculator, Character Map, Snipping Tool to do
- ✅ Magnifier, On-Screen Keyboard (KMag, KVkbd)
- 🟡 Networking UI: Wi-Fi chooser by the clock; adapter settings, Network and Sharing Center,
     Firewall, Remote Desktop client to do
- 🟡 Printing: CUPS print service + Printers and Faxes; automatic network printer discovery to do
- ⬜ Programming tools: KDevelop-like IDE (editor, build, gdb debugger front end) (TDE: KDevelop)
- ✅ Image Writer: .iso/.img to USB sticks and SD cards with a read-back check
- ✅ Firefox (Debian firefox-esr) preinstalled and the default browser
- ✅ Windows programs: double-click an .exe (Wine), Start menu → Windows Programs
- 🟡 Notifications: XP-style bubbles for AutoPlay and Wi-Fi; system-wide notifications (KNotify) to do

**Packages and system**
- 🟡 Installer: graphical wizard (disk, computer name, user) → GPT + ext4 + Limine (UEFI/BIOS);
     package and settings choices to do
- ✅ App Store: Debian and desktop packages by category, search, featured programs, install/remove
     with dependencies (apt), updates; double-click a .deb to install it
- ✅ Packages' install scripts run (real dpkg database; apt works)
- ✅ Accounts: graphical login, User Accounts (add/remove users, passwords), administrator prompts
- 🟡 Security: NX, kernel W^X, guard pages, per-user permissions done; ASLR, SMEP/SMAP to do
- 🟡 Power management: power off / restart, power button, battery and lid readings, idle CPU
     states; sleep, CPU frequency scaling, battery icon to do
- ✅ Updates (Syluxium Update: Debian and Trinity through apt) and a Recovery boot entry
     (repair disks and packages, reset settings or passwords, root prompt)

Tally: 41 done · 17 partly done · 1 to do.
