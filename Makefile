# Syluxium build system
#
#   make            build the kernel and a bootable disk image (build/syluxium.img)
#   make run        boot the image in QEMU (BIOS)
#   make run-uefi   boot the image in QEMU (UEFI, needs edk2-ovmf)
#   make debug      boot paused with a GDB stub on :1234
#   make clean      remove build output
#   make distclean  also remove downloaded dependencies

.SUFFIXES:
MAKEFLAGS += --no-builtin-rules

# ---------------------------------------------------------------------------
# Toolchain (clang targets bare-metal directly, no cross compiler needed)
# ---------------------------------------------------------------------------
CXX     := clang++
LD      := ld.lld
QEMU    := qemu-system-x86_64
OVMF    ?= /usr/share/edk2/x64/OVMF_CODE.4m.fd

# Extra compiler flags, e.g.  make EXTRA_CXXFLAGS=-DSYLUX_TEST_GUARD_FAULT
EXTRA_CXXFLAGS ?=

BUILD   := build
OBJDIR  := $(BUILD)/obj
KERNEL  := $(BUILD)/syluxium.elf
IMAGE   := $(BUILD)/syluxium.img
PICKER  := $(BUILD)/picker.efi
INITRAMFS := $(BUILD)/initramfs.cpio
ROOTFS  := $(BUILD)/rootfs

# Syluxium's own test programs: ordinary Linux binaries built with the host toolchain.
USER_CC     := gcc
# Linked against Debian's static glibc and libgcc (baseline x86-64, AVX2
# chosen at run time), not the build host's: a distribution built for x86-64-v3 (CachyOS)
# ships a libc.a whose startup code uses AVX2, so the programs died with
# SIGILL on pre-Haswell CPUs (iMac13,1).
LIBC_SYSROOT   := $(CURDIR)/deps/libc-sysroot
LIBC_SYSROOT32 := $(CURDIR)/deps/libc-sysroot32
USER_CFLAGS := --sysroot=$(LIBC_SYSROOT) -isystem $(LIBC_SYSROOT)/usr/include/x86_64-linux-gnu \
               -B$(LIBC_SYSROOT)/usr/lib/x86_64-linux-gnu/ -L$(LIBC_SYSROOT)/usr/lib/x86_64-linux-gnu \
               -B$(LIBC_SYSROOT)/usr/lib/gcc/x86_64-linux-gnu/14/ -L$(LIBC_SYSROOT)/usr/lib/gcc/x86_64-linux-gnu/14 \
               -static -O2 -Wall
TESTS_DIR   := $(BUILD)/tests
USER_PROGS  := $(patsubst userspace/%.c,$(TESTS_DIR)/%,$(wildcard userspace/*.c))
# 32-bit (i386) test programs for the compat layer (host gcc multilib, static).
USER32_CFLAGS := --sysroot=$(LIBC_SYSROOT32) -isystem $(LIBC_SYSROOT32)/usr/include/i386-linux-gnu \
                 -B$(LIBC_SYSROOT32)/usr/lib/i386-linux-gnu/ -L$(LIBC_SYSROOT32)/usr/lib/i386-linux-gnu \
                 -B$(LIBC_SYSROOT32)/usr/lib/gcc/i686-linux-gnu/14/ -L$(LIBC_SYSROOT32)/usr/lib/gcc/i686-linux-gnu/14 \
                 -m32 -static -O2 -Wall -pthread
USER32_PROGS  := $(patsubst userspace/ia32/%.c,$(TESTS_DIR)/%,$(wildcard userspace/ia32/*.c))
# Windows test programs for Wine (x86-64 and i386 PE), built with clang +
# lld-link against import libraries made from userspace/windows/*.def.
# The shell's software-rendering fix (userspace/shell/glsync.c): no libc needed.
SHELL_GLSYNC  := $(BUILD)/libsyluxium-glsync.so
# The shell's window frames (userspace/shell/syluxium-decor.cpp): a Wayfire
# plugin, built with clang against Debian's Wayfire/wlroots headers and
# libraries (a sysroot of their -dev packages in deps/shell-sysroot).
SHELL_SYSROOT := $(CURDIR)/deps/shell-sysroot
SHELL_SYSROOT_PKGS := wayfire-dev libwlroots-0.18-dev libcairo2-dev libpango1.0-dev libglm-dev libstdc++-14-dev libgles-dev libc6-dev
SHELL_PKGCONFIG := PKG_CONFIG_SYSROOT_DIR=$(SHELL_SYSROOT) PKG_CONFIG_LIBDIR=$(SHELL_SYSROOT)/usr/lib/x86_64-linux-gnu/pkgconfig:$(SHELL_SYSROOT)/usr/share/pkgconfig pkg-config
SHELL_DECOR   := $(BUILD)/libsyluxium-decor.so
WIN_PROGS     := $(TESTS_DIR)/winhello64.exe $(TESTS_DIR)/winhello32.exe $(TESTS_DIR)/winbox32.exe

# Official Debian packages (and their dependencies) that make up the userland.
DEBIAN_SUITE := trixie
# Trinity Desktop Environment (phase 9): Trinity's own Debian repository.
TRINITY_REPO := https://mirror.ppa.trinitydesktop.org/trinity/deb/trinity-r14.1.x|trixie|main,deps
TRINITY_PKGS := dbus tdebase-trinity-bin twin-trinity kicker-trinity kdesktop-trinity konsole-trinity tdm-trinity kuser-trinity tdepasswd-trinity tdesudo-trinity tdepowersave-trinity \
                konqueror-trinity kcontrol-trinity ksmserver-trinity tdebase-data-trinity kmix-trinity kmail-trinity kaffeine-trinity \
                tde-style-qtcurve-trinity twin-style-dekorator-trinity ksplash-trinity ksysguard-trinity \
                kmag-trinity kvkbd-trinity ksystemlog-trinity kate-trinity tdeprint-trinity
DEBIAN_PKGS  := bash busybox hello ncurses-base pciutils pci.ids netbase alsa-utils xserver-xorg-core xserver-xorg-video-fbdev xserver-xorg-input-evdev xinit x11-xserver-utils x11-xkb-utils xterm twm xfonts-base sudo libpam-runtime libpam-modules login fdisk e2fsprogs dosfstools xdotool firefox-esr \
                wine wine64 iw wpasupplicant wireless-regdb hostapd fonts-wine xcape gparted python3 python3-gi python3-gi-cairo fonts-inter wayfire wayfire-plugin-winshadows xwayland seatd gir1.2-gtklayershell-0.1 swaybg gir1.2-gtk-3.0 librsvg2-common cups cups-bsd dpkg apt debconf libc-bin coreutils trinity-keyring ca-certificates \
                libgl1-mesa-dri libglx-mesa0 libegl-mesa0 mesa-utils
# Right-click "Extract Here" for .7z (zip and tar are handled without it).
DEBIAN_PKGS  += 7zip
# libinput (Wayland shell input) and its "libinput list-devices / debug-events" tools (~4 MB)
DEBIAN_PKGS  += libinput-tools
# Bluetooth: BlueZ (bluetoothd, bluetoothctl, btmgmt)
DEBIAN_PKGS  += bluez
# Bluetooth audio (A2DP, HFP/HSP) for the ALSA-only system: bluealsad + the "bluealsa" ALSA device
DEBIAN_PKGS  += bluez-alsa-utils libasound2-plugin-bluez
# Broadcom b43 Wi-Fi (iMacs, older MacBooks): tools/b43-firmware.sh cuts its
# firmware out of Broadcom's driver at build time (syluxium-b43-firmware can
# still do it on the machine).
DEBIAN_CONTRIB_REPO := http://deb.debian.org/debian|$(DEBIAN_SUITE)|contrib
DEBIAN_PKGS  += b43-fwcutter
# Accessories: WordPad (AbiWord, reads and writes .rtf/.doc/.odt), notify-send;
# network printer discovery (mDNS: avahi-daemon, cups-browsed, .local names).
DEBIAN_PKGS  += abiword libnotify-bin avahi-daemon avahi-utils cups-browsed libnss-mdns
TRINITY_PKGS += kcalc-trinity kcharselect-trinity kolourpaint-trinity ksnapshot-trinity
# Xfce session: core (the Dusk desktop; Trinity stays as the "Classic" session).
DEBIAN_PKGS  += xfce4-session xfwm4 xfce4-panel xfdesktop4 xfce4-settings xfconf thunar \
                xfce4-whiskermenu-plugin volumeicon-alsa xfce4-power-manager \
                xfce4-appfinder libxfce4ui-utils gvfs gvfs-daemons xdg-user-dirs
# Xfce session: applications (GTK replacements for the Trinity apps)
# Terminal, Calculator, Paint, Character Map, Snipping Tool, text editor
# (~28 MB); yad: GTK dialogs for our shell scripts (zenity 4 would pull GTK 4
# and libadwaita, ~42 MB); xdg-utils: xdg-open / xdg-mime.
# udisks2 (pulled in by GVfs) is left out: no udev, and syluxium-automount
# mounts USB sticks.
DEBIAN_PKGS  += xfce4-terminal galculator drawing gucharmap xfce4-screenshooter mousepad yad xdg-utils
# Task Manager's window list in the Wayland shell (Xfce's window library from Python).
DEBIAN_PKGS  += gir1.2-libxfce4windowing-0.0
# X11 programs in the Wayland shell (Xwayland): xsettingsd gives them the
# Dusk theme, icons, fonts and cursor (XSETTINGS, which xfsettingsd only
# provides on a real X server); clipboard tools for both sides (xclip,
# wl-clipboard) and xprop/xwininfo/xdpyinfo (~1.5 MB). Test programs for
# xwayland-test: an SDL 2 game (sopwith) and a Qt 5 one (kcheckers).
DEBIAN_PKGS  += xsettingsd x11-utils xclip wl-clipboard sopwith kcheckers
XFCE_APPS_EXCLUDE := udisks2
DEBIAN_ROOT  := $(BUILD)/rootfs-debian
DEBIAN_STAMP := $(DEBIAN_ROOT)/.fetched
DEBIAN_CONFIGURED := $(DEBIAN_ROOT)/.configured
# A few i386 packages for testing 32-bit dynamic linking (Steam is 32-bit).
DEBIAN_I386_PKGS  := hello busybox bash
# Only the libraries these programs need (base-files would pull in gawk & co.).
# Mesa's 3D stack (~190 MB, most of it LLVM for radeonsi/llvmpipe): OpenGL for
# Xorg's GLAMOR acceleration and programs on the GPU drivers (i915: iris /
# crocus, amdgpu: radeonsi, nouveau), software rendering elsewhere.
# Wine (9-wine): libwine links the multimedia/hardware libraries below only
# from optional Unix-side modules (winegstreamer, winepulse, gphoto2, wpcap,
# capi, opencl); without them those modules don't load and the rest
# of Wine works. FFmpeg + GStreamer alone would add ~150 MB.
WINE_EXCLUDE := libavcodec61,libavformat61,libavutil59,libgstreamer-plugins-base1.0-0,libgstreamer1.0-0,libgphoto2-6t64,libgphoto2-port12t64,libpcap0.8t64,libpulse0,libcapi20-3t64,ocl-icd-libopencl1
DEBIAN_EXCLUDE := cpp,cpp-14,cpp-14-x86-64-linux-gnu,cpp-x86-64-linux-gnu,man-db,groff-base,systemd,udev,xbase-clients,systemd-sysv,libpam-systemd,dbus-user-session,$(WINE_EXCLUDE),$(XFCE_APPS_EXCLUDE)
DEBIAN_I386_EXCLUDE := base-files,debianutils,gawk,libmpfr6,libgmp10,libsigsegv2,readline-common,libreadline8t64
DEBIAN_I386_PROGS := hello busybox bash
DEBIAN_I386_ROOT  := $(BUILD)/rootfs-debian-i386
DEBIAN_I386_STAMP := $(DEBIAN_I386_ROOT)/.fetched
# 32-bit Windows programs: only the 32-bit Windows DLLs of the i386 libwine
# package (tools/wine-setup.sh puts them next to the 64-bit ones; the 64-bit
# Wine runs them in WoW64 mode, so wine32 and its i386 libraries aren't needed).
WINE_I386_ROOT  := $(BUILD)/rootfs-wine-i386
WINE_I386_STAMP := $(WINE_I386_ROOT)/.fetched

LIMINE_DIR          := deps/limine
LIMINE_PROTOCOL_DIR := deps/limine-protocol
LIMINE_BRANCH       := v11.x-binary

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
CXXFLAGS := \
    --target=x86_64-unknown-none-elf \
    -std=c++20 \
    -O2 -g \
    -Wall -Wextra -Wshadow -Wno-unused-parameter \
    -ffreestanding \
    -fno-exceptions -fno-rtti \
    -fstack-protector-strong -mstack-protector-guard=global -fno-stack-check \
    -fno-threadsafe-statics \
    -fno-pic -fno-pie \
    -fno-omit-frame-pointer \
    -nostdlibinc \
    -mcmodel=kernel \
    -mno-red-zone \
    -mno-80387 -mno-mmx -mno-sse -mno-sse2 -msoft-float \
    -Ikernel \
    -isystem $(LIMINE_PROTOCOL_DIR)/include \
    -MD -MP \
    $(EXTRA_CXXFLAGS)

# Linux drivers (C) built against the LinuxKPI headers.
CC := clang
LKPI_INCLUDE := kernel/linuxkpi/include
CFLAGS := \
    --target=x86_64-unknown-none-elf \
    -std=gnu11 \
    -O2 -g \
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-pointer-sign \
    -ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
    -fno-omit-frame-pointer -fno-strict-aliasing \
    -nostdlibinc -mcmodel=kernel -mno-red-zone \
    -mno-80387 -mno-mmx -mno-sse -mno-sse2 -msoft-float \
    -isystem $(LKPI_INCLUDE) \
    -MD -MP
CXXFLAGS += -isystem $(LKPI_INCLUDE) -isystem deps/uacpi/include -DUACPI_USE_BUILTIN_STRING

ASFLAGS := --target=x86_64-unknown-none-elf -g -Ikernel -MMD -MP

LDFLAGS := \
    -m elf_x86_64 \
    -nostdlib -static \
    --no-pie \
    --build-id=none \
    -z max-page-size=0x1000 \
    -T kernel/linker.ld

# -cpu max: the host's glibc (CachyOS builds for x86-64-v3) needs AVX2.
QEMU_FLAGS := \
    -M q35 -m 2G -smp 4 -cpu max \
    -drive file=$(IMAGE),format=raw,if=ide \
    -serial stdio \
    -no-reboot -no-shutdown

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
CXX_SRCS := $(shell find kernel -name '*.cpp')
ASM_SRCS := $(shell find kernel -name '*.S')
C_SRCS   := $(shell find kernel -name '*.c')
# uACPI (deps/uacpi, MIT): the ACPI AML interpreter, fetched at a fixed version.
UACPI_DIR     := deps/uacpi
UACPI_VERSION := 7.0.0
UACPI_SRCS := $(addprefix $(UACPI_DIR)/source/,default_handlers.c event.c interpreter.c io.c mutex.c namespace.c \
              notify.c opcodes.c opregion.c osi.c registers.c resources.c sleep.c stdlib.c tables.c types.c uacpi.c \
              utilities.c)
OBJS     := $(CXX_SRCS:%.cpp=$(OBJDIR)/%.cpp.o) $(ASM_SRCS:%.S=$(OBJDIR)/%.S.o) $(C_SRCS:%.c=$(OBJDIR)/%.c.o) \
            $(UACPI_SRCS:%.c=$(OBJDIR)/%.c.o)
DEPS     := $(OBJS:.o=.d)

.PHONY: all kernel image initramfs rootdisk run run-uefi debug clean distclean deps

all: image

kernel: $(KERNEL)
image: $(IMAGE)
initramfs: $(INITRAMFS)

# ---------------------------------------------------------------------------
# Dependencies (Limine bootloader binaries + protocol header)
# ---------------------------------------------------------------------------
deps: $(LIMINE_DIR)/limine $(LIMINE_PROTOCOL_DIR)/include/limine.h

$(LIMINE_PROTOCOL_DIR)/include/limine.h:
	git clone --depth=1 https://github.com/limine-bootloader/limine-protocol $(LIMINE_PROTOCOL_DIR)

$(LIMINE_DIR)/limine.c:
	git clone --depth=1 -b $(LIMINE_BRANCH) https://github.com/limine-bootloader/limine $(LIMINE_DIR)

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c
	$(MAKE) -C $(LIMINE_DIR) CC=cc

# ---------------------------------------------------------------------------
# Kernel
# ---------------------------------------------------------------------------
$(OBJDIR)/%.cpp.o: %.cpp | $(LIMINE_PROTOCOL_DIR)/include/limine.h
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@$(CXX) $(CXXFLAGS) -MJ $@.json -c $< -o $@

# uACPI: kernel C flags, without the LinuxKPI headers.
UACPI_CFLAGS := $(filter-out -isystem $(LKPI_INCLUDE),$(CFLAGS)) -isystem deps/uacpi/include -DUACPI_USE_BUILTIN_STRING \
                -Wno-unused-function -Wno-implicit-fallthrough
$(UACPI_SRCS): | $(UACPI_DIR)/.fetched
$(UACPI_DIR)/.fetched:
	@echo "  GIT   uACPI $(UACPI_VERSION)"
	@rm -rf $(UACPI_DIR) && git clone -q --depth 1 --branch $(UACPI_VERSION) https://github.com/uACPI/uACPI.git $(UACPI_DIR)
	@touch $@
$(OBJS): | $(UACPI_DIR)/.fetched

$(OBJDIR)/deps/uacpi/%.c.o: deps/uacpi/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(UACPI_CFLAGS) -c $< -o $@

$(OBJDIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -MJ $@.json -c $< -o $@

# Linux sources (kernel/linuxkpi: the wireless stack, drivers, lib/) see the
# LinuxKPI kernel configuration and Linux's own code-generation flags; the
# module name is the directory (cfg80211 = net/wireless, mac80211, mt7601u, ...).
LKPI_LINUX_CFLAGS := -I$(LKPI_INCLUDE) -include linux/kconfig.h -Werror=implicit-function-declaration -Werror=int-conversion \
    -Werror=incompatible-pointer-types -D__KERNEL__ -fno-strict-overflow -fno-delete-null-pointer-checks \
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-gnu-variable-sized-type-not-at-end \
    -Wno-initializer-overrides -Wno-unused-const-variable -Wno-implicit-fallthrough -Wno-format-zero-length \
    -Wno-missing-field-initializers -Wno-sign-compare -Wno-address-of-packed-member -Wno-tautological-constant-out-of-range-compare \
    -Wno-default-const-init-field-unsafe -Wno-unknown-warning-option
lkpi_modname = $(if $(findstring /brcm80211/,$<),brcmfmac,$(lkpi_modname_net))
lkpi_modname_net = $(if $(findstring /drivers/net/usb/,$<),$(basename $(notdir $<)),$(lkpi_modname_net2))
lkpi_modname_net2 = $(if $(findstring /iwlwifi/mvm/,$<),iwlmvm,$(if $(findstring /iwlwifi/,$<),iwlwifi,$(if $(findstring /ethernet/realtek/r8169,$<),r8169,$(if $(findstring /ethernet/broadcom/tg3,$<),tg3,$(if $(findstring /ethernet/intel/e1000e/,$<),e1000e,$(if $(findstring /ethernet/intel/igc/,$<),igc,$(if $(findstring /drivers/net/phy/realtek,$<),realtek,$(if $(findstring /drivers/net/phy/,$<),libphy,$(lkpi_modname_rest)))))))))
lkpi_modname_rest = $(if $(findstring /drm/i915/,$<),i915,$(if $(findstring /drm/radeon/,$<),radeon,$(if $(findstring /drm/amd/,$<),amdgpu,$(if $(findstring /drm/nouveau/,$<),nouveau,$(if $(findstring /drm/scheduler/,$<),gpu_sched,$(patsubst virtual,mac80211_hwsim,$(patsubst wireless,cfg80211,$(notdir $(patsubst %/,%,$(dir $<))))))))))
# Intel Wi-Fi (iwlwifi + iwlmvm): headers relative to the driver's root.
LKPI_IWL := kernel/linuxkpi/drivers/net/wireless/intel/iwlwifi
$(OBJDIR)/$(LKPI_IWL)/%: LKPI_DRV_CFLAGS = -I$(LKPI_IWL) -DCONFIG_IWLWIFI=1 -DCONFIG_IWLMVM=1 -include net/lkpi_iwlwifi.h
# Broadcom Wi-Fi (bcma bus, b43).
$(OBJDIR)/kernel/linuxkpi/drivers/bcma/%: LKPI_DRV_CFLAGS = -include net/lkpi_bcma.h
$(OBJDIR)/kernel/linuxkpi/drivers/net/wireless/broadcom/%: LKPI_DRV_CFLAGS = -include net/lkpi_bcma.h
$(OBJDIR)/kernel/linuxkpi/drivers/net/wireless/realtek/rtw88/%: LKPI_DRV_CFLAGS = -include net/lkpi_netcompat.h
$(OBJDIR)/kernel/linuxkpi/drivers/net/wireless/ath/%: LKPI_DRV_CFLAGS = -include net/lkpi_netcompat.h -Ikernel/linuxkpi/drivers/net/wireless/ath
# Bluetooth core
$(OBJDIR)/kernel/linuxkpi/net/bluetooth/%: LKPI_DRV_CFLAGS = -include net/lkpi_bt.h
$(OBJDIR)/kernel/linuxkpi/drivers/bluetooth/%: LKPI_DRV_CFLAGS = -include net/lkpi_bt.h -fshort-wchar
LKPI_MT76 := kernel/linuxkpi/drivers/net/wireless/mediatek/mt76
$(OBJDIR)/$(LKPI_MT76)/%: LKPI_DRV_CFLAGS = -include net/lkpi_netcompat.h -I$(LKPI_MT76) -DCONFIG_PM=1
LKPI_BRCM := kernel/linuxkpi/drivers/net/wireless/broadcom/brcm80211
$(OBJDIR)/$(LKPI_BRCM)/%: LKPI_DRV_CFLAGS = -include net/lkpi_bcma.h -I$(LKPI_BRCM)/brcmfmac -I$(LKPI_BRCM)/include
# Realtek Ethernet (r8169) and phylib.
LKPI_NETDRV := kernel/linuxkpi/drivers/net
$(OBJDIR)/$(LKPI_NETDRV)/ethernet/realtek/%: LKPI_DRV_CFLAGS = -include net/lkpi_phy.h
# Broadcom Tigon3 Ethernet (tg3): iMacs, Mac minis and MacBook Pros, Dell/HP desktops and servers.
$(OBJDIR)/$(LKPI_NETDRV)/ethernet/broadcom/%: LKPI_DRV_CFLAGS = -include net/lkpi_tg3.h
$(OBJDIR)/$(LKPI_NETDRV)/phy/%: LKPI_DRV_CFLAGS = -include net/lkpi_phy.h
# USB Ethernet adapters (r8152, ax88179_178a over usbnet)
$(OBJDIR)/$(LKPI_NETDRV)/usb/%: LKPI_DRV_CFLAGS = -include net/lkpi_usbnet.h
$(OBJDIR)/$(LKPI_NETDRV)/ethernet/intel/%: LKPI_DRV_CFLAGS = -include net/lkpi_phy.h -DCONFIG_E1000E=1 -DCONFIG_IGC=1
# GPU drivers: their own include directory and Kconfig (drm/lkpi_<driver>_config.h).
LKPI_I915 := kernel/linuxkpi/drivers/gpu/drm/i915
$(OBJDIR)/$(LKPI_I915)/%: LKPI_DRV_CFLAGS = -I$(LKPI_I915) -DI915 -include drm/lkpi_i915_config.h -gline-tables-only
LKPI_RADEON := kernel/linuxkpi/drivers/gpu/drm/radeon
$(OBJDIR)/$(LKPI_RADEON)/%: LKPI_DRV_CFLAGS = -I$(LKPI_RADEON) -include drm/lkpi_radeon_config.h -gline-tables-only
# amdgpu: its register headers (~270 MB, drivers/gpu/drm/amd/include/asic_reg)
# stay out of the repository and come from a sparse Linux 6.12 checkout.
LKPI_AMD := kernel/linuxkpi/drivers/gpu/drm/amd
LINUX_SRC := deps/linux-src
AMD_ASIC_REG := $(LINUX_SRC)/drivers/gpu/drm/amd/include/asic_reg
$(OBJDIR)/$(LKPI_AMD)/%: LKPI_DRV_CFLAGS = -I$(AMD_ASIC_REG) -I$(LKPI_AMD)/include -I$(AMD_ASIC_REG)/.. -I$(LKPI_AMD)/amdgpu -I$(LKPI_AMD)/pm/inc \
    -I$(LKPI_AMD)/acp/include -I$(LKPI_AMD)/pm/swsmu -I$(LKPI_AMD)/pm/swsmu/inc -I$(LKPI_AMD)/pm/swsmu/inc/pmfw_if \
    -I$(LKPI_AMD)/pm/swsmu/smu11 -I$(LKPI_AMD)/pm/swsmu/smu12 -I$(LKPI_AMD)/pm/swsmu/smu13 -I$(LKPI_AMD)/pm/swsmu/smu14 \
    -I$(LKPI_AMD)/pm/powerplay/inc -I$(LKPI_AMD)/pm/powerplay/smumgr -I$(LKPI_AMD)/pm/powerplay/hwmgr \
    -I$(LKPI_AMD)/pm/legacy-dpm -I$(LKPI_AMD)/amdkfd -I$(LKPI_AMD)/display -I$(LKPI_AMD)/display/include \
    -I$(LKPI_AMD)/display/modules/inc -I$(LKPI_AMD)/display/dc -I$(LKPI_AMD)/display/amdgpu_dm \
    -I$(LKPI_AMD)/display/dc/inc -I$(LKPI_AMD)/display/dc/inc/hw -I$(LKPI_AMD)/display/dc/clk_mgr \
    -I$(LKPI_AMD)/display/dc/hwss -I$(LKPI_AMD)/display/dc/resource -I$(LKPI_AMD)/display/dc/dsc \
    -I$(LKPI_AMD)/display/dc/optc -I$(LKPI_AMD)/display/dc/dpp -I$(LKPI_AMD)/display/dc/hubbub \
    -I$(LKPI_AMD)/display/dc/dccg -I$(LKPI_AMD)/display/dc/hubp -I$(LKPI_AMD)/display/dc/dio \
    -I$(LKPI_AMD)/display/dc/dwb -I$(LKPI_AMD)/display/dc/hpo -I$(LKPI_AMD)/display/dc/mmhubbub \
    -I$(LKPI_AMD)/display/dc/mpc -I$(LKPI_AMD)/display/dc/opp -I$(LKPI_AMD)/display/dc/pg \
    -I$(LKPI_AMD)/display/modules/freesync -I$(LKPI_AMD)/display/modules/color \
    -I$(LKPI_AMD)/display/modules/info_packet -I$(LKPI_AMD)/display/modules/power \
    -I$(LKPI_AMD)/display/dmub/inc -I$(LKPI_AMD)/display/modules/hdcp -I$(LKPI_AMD)/display/dc/dml2 \
    -I$(LKPI_AMD)/display/dc/dml2/dml21 -I$(LKPI_AMD)/display/dc/dml2/dml21/inc \
    -I$(LKPI_AMD)/display/dc/dml2/dml21/src/dml2_core -I$(LKPI_AMD)/display/dc/dml2/dml21/src/dml2_dpmm \
    -I$(LKPI_AMD)/display/dc/dml2/dml21/src/dml2_mcg -I$(LKPI_AMD)/display/dc/dml2/dml21/src/dml2_pmo \
    -I$(LKPI_AMD)/display/dc/dml2/dml21/src/dml2_standalone_libraries \
    -I$(LKPI_AMD)/display/dc/dml2/dml21/src/inc \
    -DBUILD_FEATURE_TIMING_SYNC=0 -include drm/lkpi_amdgpu_dc_config.h -gline-tables-only
# The display core's bandwidth maths use floating point: those files get SSE2,
# and run inside kernel FPU sections (DC_FP_START/END -> kernel_fpu_begin/end).
-include $(LKPI_AMD)/display/lkpi-fpu-objs.mk
$(AMD_DC_FPU_OBJS): LKPI_FP_CFLAGS = -msse -msse2 -mno-soft-float
LKPI_NOUVEAU := kernel/linuxkpi/drivers/gpu/drm/nouveau
$(OBJDIR)/$(LKPI_NOUVEAU)/%: LKPI_DRV_CFLAGS = -I$(LKPI_NOUVEAU)/include -I$(LKPI_NOUVEAU)/include/nvkm -I$(LKPI_NOUVEAU)/nvkm \
    -I$(LKPI_NOUVEAU) -include drm/lkpi_nouveau_config.h -gline-tables-only
$(filter $(OBJDIR)/$(LKPI_AMD)/%,$(OBJS)): | $(AMD_ASIC_REG)/.fetched
$(AMD_ASIC_REG)/.fetched:
	@echo "  GIT   Linux 6.12 amdgpu register headers"
	@if [ ! -d $(LINUX_SRC)/.git ]; then rm -rf $(LINUX_SRC) && git clone -q --depth 1 --branch v6.12 --filter=blob:none --sparse \
	    https://github.com/torvalds/linux.git $(LINUX_SRC); fi
	@cd $(LINUX_SRC) && git sparse-checkout add drivers/gpu/drm/amd/include/asic_reg
	@touch $@
$(OBJDIR)/kernel/linuxkpi/%.c.o: kernel/linuxkpi/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(filter-out -isystem $(LKPI_INCLUDE),$(CFLAGS)) $(LKPI_LINUX_CFLAGS) $(LKPI_DRV_CFLAGS) $(LKPI_FP_CFLAGS) -DKBUILD_MODNAME='"$(lkpi_modname)"' -DKBUILD_BASENAME='"$(basename $(notdir $<))"' \
	    -MJ $@.json -c $< -o $@

$(OBJDIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(CXX) $(ASFLAGS) -MJ $@.json -c $< -o $@

# Every kernel names its build in the boot log (commit, "+changes" when built
# from uncommitted code, build time), and a copy of each is kept in
# kernel-builds/ (git-ignored): call chains from a tester's log (lockups,
# Alt+SysRq+W) only resolve with that exact syluxium.elf.
BUILD_ID = $(shell git describe --always --dirty=+changes 2>/dev/null || echo unknown)
BUILD_TIME = $(shell date -u '+%Y%m%d-%H%M')
$(KERNEL): $(OBJS) kernel/linker.ld
	@echo "  LD    $@"
	@printf 'const char syluxium_build_id[] = "%s, built %s UTC";\n' "$(BUILD_ID)" "$(BUILD_TIME)" > $(OBJDIR)/version.c
	@$(CC) $(CFLAGS) -c $(OBJDIR)/version.c -o $(OBJDIR)/version.o
	@$(LD) $(LDFLAGS) $(OBJS) $(OBJDIR)/version.o -o $@
	@mkdir -p kernel-builds && cp $@ "kernel-builds/syluxium-$(BUILD_ID)-$(BUILD_TIME).elf"
	@ls -t kernel-builds/*.elf | tail -n +31 | xargs -r rm -f  # the newest 30
	@# compile_commands.json for CLion / clangd
	@{ echo '['; find $(OBJDIR) -name '*.o.json' -exec cat {} + | sed '$$ s/,$$//'; echo ']'; } > compile_commands.json

# ---------------------------------------------------------------------------
# Initramfs: the root filesystem the kernel unpacks at boot (cpio, newc)
# ---------------------------------------------------------------------------
$(LIBC_SYSROOT)/.done:
	@echo "  FETCH Debian libc6-dev (amd64) for the test programs"
	@python3 tools/debian-fetch.py --out $(LIBC_SYSROOT) --cache deps/debian --no-deps libc6-dev libc6 linux-libc-dev libgcc-14-dev >/dev/null
	@touch $@

$(LIBC_SYSROOT32)/.done:
	@echo "  FETCH Debian libc6-dev (i386) for the 32-bit test programs"
	@python3 tools/debian-fetch.py --out $(LIBC_SYSROOT32) --cache deps/debian --arch i386 --no-deps libc6-dev libc6 linux-libc-dev libgcc-14-dev >/dev/null
	@touch $@

$(TESTS_DIR)/%: userspace/%.c | $(LIBC_SYSROOT)/.done
	@mkdir -p $(dir $@)
	@echo "  USER  $<"
	@$(USER_CC) $(USER_CFLAGS) $< -o $@ -lm

$(SHELL_SYSROOT)/.done:
	@echo "  FETCH Debian Wayfire/wlroots development files for the shell's plugin"
	@python3 tools/debian-fetch.py --suite $(DEBIAN_SUITE) --out $(SHELL_SYSROOT) --cache deps/debian \
	    --exclude systemd,udev,systemd-sysv,libpam-systemd,dbus-user-session,man-db $(SHELL_SYSROOT_PKGS) >/dev/null
	@touch $@

$(SHELL_DECOR): userspace/shell/syluxium-decor.cpp | $(SHELL_SYSROOT)/.done
	@echo "  SO    $<"
	@clang++ --target=x86_64-linux-gnu --sysroot=$(SHELL_SYSROOT) --gcc-install-dir=$(SHELL_SYSROOT)/usr/lib/gcc/x86_64-linux-gnu/14 \
	    -std=c++17 -O2 -fPIC -shared -DWLR_USE_UNSTABLE -DWAYFIRE_PLUGIN -Wall -Wno-deprecated-declarations \
	    $$($(SHELL_PKGCONFIG) --cflags wayfire wlroots-0.18 cairo pango pangocairo glesv2) \
	    $< -o $@ -fuse-ld=lld -Wl,--as-needed $$($(SHELL_PKGCONFIG) --libs cairo pango pangocairo glesv2)

$(SHELL_GLSYNC): userspace/shell/glsync.c
	@echo "  SO    $<"
	@$(USER_CC) -shared -fPIC -nostdlib -O2 -Wall $< -o $@

$(TESTS_DIR)/%: userspace/ia32/%.c | $(LIBC_SYSROOT32)/.done
	@mkdir -p $(dir $@)
	@echo "  USER32 $<"
	@$(USER_CC) $(USER32_CFLAGS) $< -o $@

$(TESTS_DIR)/winhello64.exe: userspace/windows/winhello.c userspace/windows/kernel32-x86_64.def userspace/windows/user32-x86_64.def
	@mkdir -p $(TESTS_DIR)/win64
	@echo "  WIN64 $<"
	@llvm-dlltool -m i386:x86-64 -d userspace/windows/kernel32-x86_64.def -l $(TESTS_DIR)/win64/kernel32.lib
	@llvm-dlltool -m i386:x86-64 -d userspace/windows/user32-x86_64.def -l $(TESTS_DIR)/win64/user32.lib
	@$(CC) --target=x86_64-pc-windows-msvc -O2 -fno-stack-protector -c $< -o $(TESTS_DIR)/win64/winhello.obj
	@lld-link -nodefaultlib -entry:start -subsystem:console -out:$@ $(TESTS_DIR)/win64/winhello.obj \
	    $(TESTS_DIR)/win64/kernel32.lib $(TESTS_DIR)/win64/user32.lib

$(TESTS_DIR)/winhello32.exe: userspace/windows/winhello.c userspace/windows/kernel32-i386.def userspace/windows/user32-i386.def
	@mkdir -p $(TESTS_DIR)/win32
	@echo "  WIN32 $<"
	@llvm-dlltool -m i386 -k -d userspace/windows/kernel32-i386.def -l $(TESTS_DIR)/win32/kernel32.lib
	@llvm-dlltool -m i386 -k -d userspace/windows/user32-i386.def -l $(TESTS_DIR)/win32/user32.lib
	@$(CC) --target=i686-pc-windows-msvc -O2 -fno-stack-protector -c $< -o $(TESTS_DIR)/win32/winhello.obj
	@lld-link -nodefaultlib -entry:start@0 -subsystem:console -out:$@ $(TESTS_DIR)/win32/winhello.obj \
	    $(TESTS_DIR)/win32/kernel32.lib $(TESTS_DIR)/win32/user32.lib

$(TESTS_DIR)/winbox32.exe: $(TESTS_DIR)/winhello32.exe
	@echo "  WIN32 $@"
	@$(CC) --target=i686-pc-windows-msvc -O2 -fno-stack-protector -DALWAYS_BOX -c userspace/windows/winhello.c \
	    -o $(TESTS_DIR)/win32/winbox.obj
	@lld-link -nodefaultlib -entry:start@0 -subsystem:windows -out:$@ $(TESTS_DIR)/win32/winbox.obj \
	    $(TESTS_DIR)/win32/kernel32.lib $(TESTS_DIR)/win32/user32.lib

$(DEBIAN_STAMP): tools/debian-fetch.py Makefile
	@echo "  DEB   $(DEBIAN_PKGS) (+ dependencies, Debian $(DEBIAN_SUITE))"
	@python3 tools/debian-fetch.py --suite $(DEBIAN_SUITE) --out $(DEBIAN_ROOT) --cache deps/debian --dpkg-db --exclude $(DEBIAN_EXCLUDE) --repo "$(TRINITY_REPO)" --repo "$(DEBIAN_CONTRIB_REPO)" $(DEBIAN_PKGS) $(TRINITY_PKGS)
	@touch $@

# Debian's install scripts, run once per package set (fake root, no sudo).
$(DEBIAN_CONFIGURED): $(DEBIAN_STAMP) tools/configure-packages.sh
	@echo "  DPKG  dpkg --configure -a (install scripts)"
	@tools/configure-packages.sh $(DEBIAN_ROOT) $(BUILD)/configure-packages.log
	@touch $@

# Device firmware from Debian's non-free-firmware section: the packages are
# huge (GPU firmware), so only the files our drivers load are kept.
FIRMWARE_REPO  := http://deb.debian.org/debian|$(DEBIAN_SUITE)|non-free-firmware
FIRMWARE_PKGS  := firmware-mediatek firmware-realtek firmware-misc-nonfree firmware-intel-graphics firmware-amd-graphics firmware-nvidia-graphics firmware-iwlwifi firmware-brcm80211 firmware-atheros
FIRMWARE_FILES = mediatek/mt7601u.bin rtlwifi/rtl8188eufw.bin rtlwifi/rtl8723bu_nic.bin rtl_bt/rtl8723b_fw.bin \
                  $(I915_FIRMWARE)
# i915: display microcontroller (DMC) firmware for Skylake..Alder Lake, GuC/HuC
# where i915 loads them by default (Alder Lake; Tiger/Rocket Lake run without).
# Haswell/Broadwell need none. (~1.5 MB)
I915_FIRMWARE  := i915/skl_dmc_ver1_27.bin i915/kbl_dmc_ver1_04.bin i915/bxt_dmc_ver1_07.bin i915/glk_dmc_ver1_04.bin \
                  i915/icl_dmc_ver1_09.bin i915/tgl_dmc_ver2_12.bin i915/rkl_dmc_ver2_03.bin i915/adls_dmc_ver2_01.bin \
                  i915/adlp_dmc.bin i915/adlp_guc_70.bin i915/tgl_guc_70.bin i915/tgl_huc.bin
# radeon (r100 .. Southern Islands): all of it (4 MB). amdgpu without its
# display core runs Volcanic Islands / Polaris: Tonga, Fiji, Polaris 10/11/12,
# VegaM and the Carrizo/Stoney APUs (~13 MB).
GPU_FIRMWARE   := radeon/* amdgpu/topaz_* amdgpu/tonga_* amdgpu/fiji_* amdgpu/carrizo_* amdgpu/stoney_* \
                  amdgpu/polaris10_* amdgpu/polaris11_* amdgpu/polaris12_* amdgpu/vegam_* nvidia/*/*/*
# nouveau: Kepler and older need no firmware; Maxwell 2 .. Ada need NVIDIA's
# signed firmware for acceleration (2.8 MB without the 25 MB GSP images)
# amdgpu with its display core: Vega 10/12/20, Raven/Picasso/Renoir/Cezanne/
# Rembrandt/Phoenix APUs, Van Gogh, Navi 1x/2x/3x/4x (~85 MB, ~27 MB gzipped);
# datacenter parts and the ISP (camera) are left out (GPU_FIRMWARE_SKIP)
GPU_FIRMWARE   += amdgpu/vega10_* amdgpu/vega12_* amdgpu/vega20_* amdgpu/raven* amdgpu/picasso_* amdgpu/renoir_* \
                  amdgpu/green_sardine_* amdgpu/yellow_carp_* amdgpu/navi1* amdgpu/sienna_cichlid_* amdgpu/navy_flounder_* \
                  amdgpu/dimgrey_cavefish_* amdgpu/beige_goby_* amdgpu/vangogh_* amdgpu/gc_1[0-2]_* amdgpu/psp_1[0-4]_* \
                  amdgpu/sdma_[5-7]_* amdgpu/smu_1[3-4]_* amdgpu/dcn_* amdgpu/vcn_[3-5]_* amdgpu/vpe_*
# Network adapters (shell patterns, like GPU_FIRMWARE). Intel Wi-Fi: for each
# family iwlmvm drives, the newest firmware API Linux 6.12 supports (7260 ..
# AX211/BE200; ~30 MB of Debian's ~96 MB), plus the PNVM files.
# Realtek Ethernet (r8169): the chip patch files. Broadcom FullMAC (brcmfmac):
# the PCIe chips (BCM43602 of 2015-17 MacBook Pros, 4350/4356/4358/43570/4371
# ...) and the USB ones (~6.5 MB); Apple's 4355/4364 (T2 Macs) aren't in it.
# Realtek rtw88 (RTL8822BE/CE, 8821CE, 8723DE): ~0.6 MB.
# MediaTek MT7921/MT7922 (mt7921e): ~2 MB.
# Qualcomm ath10k: the laptop and PCIe-card chips QCA6174, QCA9377, QCA988x, QCA9887 (~6.5 MB).
# Bluetooth (btusb): Intel (ibt-*, ~17 MB), Realtek (rtl_bt, ~1.5 MB), MediaTek MT7921/MT7922 (~1 MB).
NET_FIRMWARE   := tigon/* intel/ibt-* rtl_bt/* mediatek/BT_RAM_CODE_MT7961_1_2_hdr.bin mediatek/BT_RAM_CODE_MT7922_1_1_hdr.bin ath10k/QCA6174/*/* ath10k/QCA9377/*/* ath10k/QCA988X/*/* ath10k/QCA9887/*/* mediatek/WIFI_RAM_CODE_MT7961_1.bin mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin mediatek/WIFI_RAM_CODE_MT7922_1.bin mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin rtw88/rtw8822b_fw.bin rtw88/rtw8822c_fw.bin rtw88/rtw8821c_fw.bin rtw88/rtw8723d_fw.bin brcm/brcmfmac*-pcie.* brcm/brcmfmac43143.bin brcm/brcmfmac43236b.bin brcm/brcmfmac43242a.bin \
                  brcm/brcmfmac43569.bin brcm/brcmfmac4373.bin \
                  iwlwifi-7260-17.ucode iwlwifi-3160-17.ucode iwlwifi-7265-17.ucode iwlwifi-7265D-29.ucode \
                  iwlwifi-3168-29.ucode iwlwifi-8000C-36.ucode iwlwifi-8265-36.ucode iwlwifi-9000-pu-b0-jf-b0-46.ucode \
                  iwlwifi-9260-th-b0-jf-b0-46.ucode iwlwifi-cc-a0-77.ucode iwlwifi-Qu-?0-??-b0-77.ucode \
                  iwlwifi-QuZ-a0-??-b0-77.ucode iwlwifi-so-a0-hr-b0-89.ucode iwlwifi-so-a0-jf-b0-77.ucode \
                  iwlwifi-so-a0-gf-a0-89.ucode iwlwifi-so-a0-gf4-a0-89.ucode iwlwifi-ty-a0-gf-a0-89.ucode \
                  iwlwifi-ma-b0-*-89.ucode iwlwifi-bz-b0-gf-a0-92.ucode iwlwifi-bz-b0-fm-c0-93.ucode \
                  iwlwifi-gl-c0-fm-c0-92.ucode iwlwifi-*.pnvm rtl_nic/*
GPU_FIRMWARE_SKIP := */gsp/* */gc_9_4_* */psp_13_0_6_* */psp_13_0_14_* */smu_13_0_6_* */smu_13_0_14_* */sdma_4_4_* */vcn_4_0_3_*
FIRMWARE_ROOT  := $(BUILD)/firmware-debian
FIRMWARE_STAMP := $(FIRMWARE_ROOT)/.fetched

$(FIRMWARE_STAMP): tools/debian-fetch.py Makefile
	@echo "  DEB   $(FIRMWARE_PKGS) (firmware: keeping $(words $(FIRMWARE_FILES)) files)"
	@python3 tools/debian-fetch.py --suite $(DEBIAN_SUITE) --out $(FIRMWARE_ROOT) --cache deps/debian --exclude $(DEBIAN_EXCLUDE) --repo "$(FIRMWARE_REPO)" $(FIRMWARE_PKGS)
	@touch $@

$(DEBIAN_I386_STAMP): tools/debian-fetch.py Makefile
	@echo "  DEB   $(DEBIAN_I386_PKGS) (i386, Debian $(DEBIAN_SUITE))"
	@python3 tools/debian-fetch.py --arch i386 --suite $(DEBIAN_SUITE) --out $(DEBIAN_I386_ROOT) --cache deps/debian --exclude $(DEBIAN_I386_EXCLUDE) $(DEBIAN_I386_PKGS)
	@touch $@

$(WINE_I386_STAMP): tools/debian-fetch.py Makefile
	@echo "  DEB   libwine (i386: 32-bit Windows DLLs)"
	@python3 tools/debian-fetch.py --arch i386 --suite $(DEBIAN_SUITE) --out $(WINE_I386_ROOT) --cache deps/debian --no-deps libwine
	@touch $@

$(INITRAMFS): $(KERNEL) $(PICKER) $(DEBIAN_CONFIGURED) tools/merge-accounts.py tools/mkfontdir.py tools/debrand-tde.py tools/syluxium_names.py tools/run-triggers.sh tools/wine-setup.sh tools/wine-prefix-links.py $(DEBIAN_STAMP) $(DEBIAN_I386_STAMP) $(WINE_I386_STAMP) $(FIRMWARE_STAMP) $(USER_PROGS) $(USER32_PROGS) $(WIN_PROGS) $(SHELL_GLSYNC) $(SHELL_DECOR) $(shell find rootfs -type f 2>/dev/null) $(shell find rootfs -type d 2>/dev/null) Makefile
	@echo "  ROOT  $(ROOTFS)"
	@rm -rf $(ROOTFS)
	@cp -a $(DEBIAN_ROOT) $(ROOTFS)
	@rm -f $(ROOTFS)/.fetched $(ROOTFS)/.configured
	@# busybox provides the basic commands (ls, cat, grep, ...) that coreutils would
	@for applet in $$($(DEBIAN_ROOT)/usr/bin/busybox --list); do \
	    [ -e $(ROOTFS)/usr/bin/$$applet ] || [ -L $(ROOTFS)/usr/bin/$$applet ] || [ -e $(ROOTFS)/usr/sbin/$$applet ] || \
	        ln -s busybox $(ROOTFS)/usr/bin/$$applet; done
	@ln -sf busybox $(ROOTFS)/usr/bin/sh
	@mkdir -p $(ROOTFS)/opt/syluxium/tests
	@cp $(USER_PROGS) $(USER32_PROGS) $(WIN_PROGS) $(ROOTFS)/opt/syluxium/tests/
	@mkdir -p $(ROOTFS)/usr/lib/syluxium && cp $(SHELL_GLSYNC) $(ROOTFS)/usr/lib/syluxium/
	@cp $(SHELL_DECOR) $(ROOTFS)/usr/lib/x86_64-linux-gnu/wayfire/
	@# 32-bit Debian userland (multiarch, as on Debian): libraries in
	@# /usr/lib/i386-linux-gnu, ld-linux.so.2, programs installed as <name>-i386
	@mkdir -p $(ROOTFS)/usr/lib/i386-linux-gnu
	@cp -a $(DEBIAN_I386_ROOT)/usr/lib/i386-linux-gnu/. $(ROOTFS)/usr/lib/i386-linux-gnu/
	@rm -rf $(ROOTFS)/usr/lib/i386-linux-gnu/gconv   # iconv modules: 8 MiB nobody needs here
	@cp -a $(DEBIAN_I386_ROOT)/usr/lib/ld-linux.so.2 $(ROOTFS)/usr/lib/
	@for prog in $(DEBIAN_I386_PROGS); do cp $(DEBIAN_I386_ROOT)/usr/bin/$$prog $(ROOTFS)/usr/bin/$$prog-i386; done
	@cp -r --remove-destination rootfs/. $(ROOTFS)/
	@python3 tools/merge-accounts.py $(ROOTFS) $(DEBIAN_ROOT)
	@# Menu entries read "Control Panel", "Start Menu", ... rather than TDE's names.
	@python3 tools/debrand-tde.py $(ROOTFS)
	@# What Debian's install scripts and triggers would build (MIME types, image loaders, schemas, icons).
	@tools/run-triggers.sh $(ROOTFS)
	@# /var/run and /var/lock point into /run, as on Debian (programs look for
	@# their sockets and pid files there: wpa_cli, dbus, ...).
	@rm -rf $(ROOTFS)/var/run $(ROOTFS)/var/lock && ln -s /run $(ROOTFS)/var/run && ln -s /run/lock $(ROOTFS)/var/lock
	@# DNS servers: the kernel's /run/resolv.conf (filled in by DHCP).
	@rm -f $(ROOTFS)/etc/resolv.conf && ln -s /run/resolv.conf $(ROOTFS)/etc/resolv.conf
	@for f in $(FIRMWARE_FILES); do \
	    src=$$(ls $(FIRMWARE_ROOT)/usr/lib/firmware/$$f $(FIRMWARE_ROOT)/lib/firmware/$$f 2>/dev/null | head -1); \
	    [ -n "$$src" ] || { echo "  warning: firmware $$f not found"; continue; }; \
	    mkdir -p $(ROOTFS)/usr/lib/firmware/$$(dirname $$f); cp -L "$$src" $(ROOTFS)/usr/lib/firmware/$$f; done
	@# GPU firmware sets (shell patterns, see GPU_FIRMWARE)
	@cd $(FIRMWARE_ROOT)/usr/lib/firmware && for f in $(GPU_FIRMWARE) $(NET_FIRMWARE); do \
	    [ -f "$$f" ] || continue; skip=; for k in $(GPU_FIRMWARE_SKIP); do case $$f in $$k) skip=1;; esac; done; [ -z "$$skip" ] || continue; \
	    mkdir -p $(CURDIR)/$(ROOTFS)/usr/lib/firmware/$$(dirname $$f); \
	    cp -L "$$f" $(CURDIR)/$(ROOTFS)/usr/lib/firmware/$$f; done
	@# Firmware that is a link to another file in the same set stays a link
	@# (Intel's Bluetooth firmware: 63 links, 17 MB if copied twice).
	@cd $(FIRMWARE_ROOT)/usr/lib/firmware && for f in $(GPU_FIRMWARE) $(NET_FIRMWARE); do \
	    [ -L "$$f" ] || continue; t=$$(readlink "$$f"); case $$t in /*|*..*) continue;; esac; \
	    [ -e "$(CURDIR)/$(ROOTFS)/usr/lib/firmware/$$(dirname $$f)/$$t" ] || continue; \
	    ln -sf "$$t" "$(CURDIR)/$(ROOTFS)/usr/lib/firmware/$$f"; done
	@ln -sf mediatek/mt7601u.bin $(ROOTFS)/usr/lib/firmware/mt7601u.bin # older drivers ask for it at the top level
	@# Broadcom b43 (older MacBooks, iMacs): cut from Broadcom's driver at build time.
	@tools/b43-firmware.sh $(ROOTFS)
	@# Wine: 32-bit DLLs (WoW64), stripped DLLs, the template prefix.
	@tools/wine-setup.sh $(ROOTFS) $(WINE_I386_ROOT)
	@# What the installer puts on the new disk: kernel, Limine (UEFI + BIOS stages), its installer.
	@mkdir -p $(ROOTFS)/usr/lib/syluxium/boot
	@cp $(KERNEL) $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/limine-bios.sys $(ROOTFS)/usr/lib/syluxium/boot/
	@cp limine-splash.png $(ROOTFS)/usr/lib/syluxium/boot/splash.png
	@cp $(PICKER) $(ROOTFS)/usr/lib/syluxium/boot/picker.efi
	@$(USER_CC) $(USER_CFLAGS) -w -o $(ROOTFS)/usr/lib/syluxium/boot/limine $(LIMINE_DIR)/limine.c
	@mkdir -p $(ROOTFS)/tmp $(ROOTFS)/var/tmp && chmod 1777 $(ROOTFS)/tmp $(ROOTFS)/var/tmp
	@# The dynamic loader's cache (ldconfig): TDE's libraries live in /opt/trinity/lib.
	@ldconfig -r $(ROOTFS) -X 2>/dev/null || echo "  warning: ldconfig failed; /opt/trinity libraries may not be found"
	@# Font indexes Debian's install scripts would have made (mkfontdir).
	@python3 tools/mkfontdir.py $(ROOTFS)/usr/share/fonts/X11/misc $(ROOTFS)/usr/share/fonts/X11/100dpi $(ROOTFS)/usr/share/fonts/X11/75dpi > /dev/null
	@echo "  CPIO  $@"
	@# gzip: 670 MB -> ~250 MB to load from the boot medium; the kernel
	@# decompresses and unpacks it in one stream.
	@cd $(ROOTFS) && find . | LC_ALL=C sort | cpio -o -H newc --quiet --owner=0:0 | gzip -n -6 > ../initramfs.cpio

# ---------------------------------------------------------------------------
# Bootable MBR disk image, BIOS + UEFI boot, built without root (sfdisk,
# mtools, mke2fs -d):
#   1. FAT32 EFI system partition: picker, Limine, kernel, and the initramfs
#      for the "whole system in memory" fallback entry
#   2. SYLUXLOGS (FAT32): boot logs, one folder per boot
#   3. SYLUXLIVE (ext4): the system itself. The default entries boot with
#      root=LABEL=SYLUXLIVE: files are read from the stick when needed, so
#      boots don't wait for the firmware to load ~600 MiB and RAM stays free.
# ---------------------------------------------------------------------------
$(IMAGE): $(KERNEL) $(INITRAMFS) $(PICKER) limine.conf limine-splash.png $(LIMINE_DIR)/limine
	@echo "  IMG   $@"
	@# Sized to fit: kernel + initramfs + 32 MiB headroom, at least 128 MiB.
	@# Built as $@.tmp and renamed only on success, so a failed build never
	@# leaves a broken image that make would consider up to date.
	@set -e; tmp=$@.tmp; rm -f $$tmp; \
	 mb=$$(( ($$(stat -c %s $(KERNEL)) + $$(stat -c %s $(INITRAMFS))) / 1048576 + 32 )); \
	 [ $$mb -lt 128 ] && mb=128; \
	 dd if=/dev/zero of=$$tmp bs=1M count=$$mb status=none; \
	 dd if=/dev/zero bs=1M count=40 status=none >> $$tmp; \
	 p1=$$(( (mb - 1) * 2048 )); \
	 p3=$$(( 2048 + p1 + 40 * 2048 )); \
	 livemb=$$(( $$(du -sm $(ROOTFS) | cut -f1) * 6 / 5 + 400 )); \
	 truncate -s $$(( (p3 / 2048 + livemb + 1) ))M $$tmp; \
	 echo 'label: dos' | sfdisk --quiet $$tmp; \
	 echo "start=2048, size=$$p1, type=ef, bootable" | sfdisk --quiet --append $$tmp; \
	 echo "start=$$(( 2048 + p1 )), size=$$(( 40 * 2048 )), type=c" | sfdisk --quiet --append $$tmp; \
	 echo "start=$$p3, size=$$(( livemb * 2048 )), type=83" | sfdisk --quiet --append $$tmp; \
	 fakeroot mke2fs -q -t ext4 -b 4096 -L SYLUXLIVE -d $(ROOTFS) -E offset=$$(( p3 * 512 )) $$tmp $$(( livemb * 256 )); \
	 logs=$$tmp@@$$(( (2048 + p1) * 512 )); \
	 mformat -i $$logs -T $$(( 40 * 2048 )) -F -c 1 -v SYLUXLOGS ::; \
	 printf 'Syluxium writes its boot log here (syluxium-log.txt).\r\nSend that file when reporting a problem.\r\n' > $$tmp.txt; \
	 mcopy -i $$logs $$tmp.txt ::/README.TXT; rm -f $$tmp.txt; \
	 mformat -i $$tmp@@1M -T $$p1 -F -v SYLUXIUM ::; \
	 mmd   -i $$tmp@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine; \
	 mcopy -i $$tmp@@1M $(KERNEL) ::/boot/syluxium.elf; \
	 mcopy -i $$tmp@@1M $(INITRAMFS) ::/boot/initramfs.cpio; \
	 mcopy -i $$tmp@@1M limine.conf $(LIMINE_DIR)/limine-bios.sys limine-splash.png ::/boot/limine; \
	 mren  -i $$tmp@@1M ::/boot/limine/limine-splash.png ::/boot/limine/splash.png; \
	 mcopy -i $$tmp@@1M $(PICKER) ::/EFI/BOOT/BOOTX64.EFI; \
	 mcopy -i $$tmp@@1M $(LIMINE_DIR)/BOOTX64.EFI ::/EFI/BOOT/limine.efi; \
	 $(LIMINE_DIR)/limine bios-install --quiet $$tmp; \
	 mv $$tmp $@

# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------
# An installed-system disk: the whole root file system on an ext4 volume
# labelled SYLUXROOT (boot it with root=LABEL=SYLUXROOT and no initramfs;
# tools/smoke-test.sh does that with ROOTDISK=build/syluxium-root.img).
ROOTDISK_IMG := $(BUILD)/syluxium-root.img
rootdisk: $(INITRAMFS)
	@rm -f $(ROOTDISK_IMG).tmp
	@truncate -s $$(( ($$(du -sm $(ROOTFS) | cut -f1) * 2 + 512) ))M $(ROOTDISK_IMG).tmp
	@fakeroot mke2fs -q -t ext4 -b 4096 -L SYLUXROOT -d $(ROOTFS) $(ROOTDISK_IMG).tmp  # fakeroot: files owned by root, not the builder
	@mv $(ROOTDISK_IMG).tmp $(ROOTDISK_IMG)
	@echo "  ROOTDISK $(ROOTDISK_IMG)"

run: $(IMAGE)
	$(QEMU) $(QEMU_FLAGS)

run-uefi: $(IMAGE)
	$(QEMU) $(QEMU_FLAGS) -drive if=pflash,format=raw,readonly=on,file=$(OVMF)

debug: $(IMAGE)
	$(QEMU) $(QEMU_FLAGS) -s -S -d int,cpu_reset -D $(BUILD)/qemu.log

clean:
	rm -rf $(BUILD) compile_commands.json

distclean: clean
	rm -rf deps

-include $(DEPS)

# The graphical boot picker (UEFI): \EFI\BOOT\BOOTX64.EFI, Limine next to it as limine.efi.
$(PICKER): boot/picker/picker.c boot/picker/efi.h boot/picker/assets.h
	@mkdir -p $(BUILD)
	@echo "  EFI   $@"
	@clang --target=x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector \
	    -mno-stack-arg-probe -fno-builtin -O2 -Wall -Wno-unused-function -c -o $(BUILD)/picker.o $<
	@lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib -out:$@ $(BUILD)/picker.o
