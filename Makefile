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
INITRAMFS := $(BUILD)/initramfs.cpio
ROOTFS  := $(BUILD)/rootfs

# Syluxium's own test programs: ordinary Linux binaries built with the host toolchain.
USER_CC     := gcc
USER_CFLAGS := -static -O2 -Wall
TESTS_DIR   := $(BUILD)/tests
USER_PROGS  := $(patsubst userspace/%.c,$(TESTS_DIR)/%,$(wildcard userspace/*.c))
# 32-bit (i386) test programs for the compat layer (host gcc multilib, static).
USER32_CFLAGS := -m32 -static -O2 -Wall -pthread
USER32_PROGS  := $(patsubst userspace/ia32/%.c,$(TESTS_DIR)/%,$(wildcard userspace/ia32/*.c))

# Official Debian packages (and their dependencies) that make up the userland.
DEBIAN_SUITE := trixie
# Trinity Desktop Environment (phase 9): Trinity's own Debian repository.
TRINITY_REPO := https://mirror.ppa.trinitydesktop.org/trinity/deb/trinity-r14.1.x|trixie|main,deps
TRINITY_PKGS := dbus tdebase-trinity-bin twin-trinity kicker-trinity kdesktop-trinity konsole-trinity tdm-trinity kuser-trinity tdepasswd-trinity tdesudo-trinity tdepowersave-trinity \
                konqueror-trinity kcontrol-trinity ksmserver-trinity tdebase-data-trinity
DEBIAN_PKGS  := bash busybox hello ncurses-base pciutils pci.ids netbase alsa-utils xserver-xorg-core xserver-xorg-video-fbdev xserver-xorg-input-evdev xinit x11-xserver-utils x11-xkb-utils xterm twm xfonts-base sudo libpam-runtime libpam-modules login fdisk e2fsprogs dosfstools xdotool firefox-esr
DEBIAN_ROOT  := $(BUILD)/rootfs-debian
DEBIAN_STAMP := $(DEBIAN_ROOT)/.fetched
# A few i386 packages for testing 32-bit dynamic linking (Steam is 32-bit).
DEBIAN_I386_PKGS  := hello busybox bash
# Only the libraries these programs need (base-files would pull in gawk & co.).
# Mesa's 3D stack (~190 MB) is only used by Xorg's GLAMOR acceleration, which the
# fbdev driver doesn't use; it returns with real GPU drivers (phase 10).
DEBIAN_EXCLUDE := mesa-libgallium,libllvm19,libz3-4,libgl1-mesa-dri,libglx-mesa0,libegl-mesa0,cpp,cpp-14,cpp-14-x86-64-linux-gnu,cpp-x86-64-linux-gnu,man-db,groff-base,systemd,udev,debconf,debconf-2.0,xbase-clients,systemd-sysv,libpam-systemd,dbus-user-session
DEBIAN_I386_EXCLUDE := base-files,debianutils,gawk,libmpfr6,libgmp10,libsigsegv2,readline-common,libreadline8t64
DEBIAN_I386_PROGS := hello busybox bash
DEBIAN_I386_ROOT  := $(BUILD)/rootfs-debian-i386
DEBIAN_I386_STAMP := $(DEBIAN_I386_ROOT)/.fetched

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
    -fno-stack-protector -fno-stack-check \
    -fno-threadsafe-statics \
    -fno-pic -fno-pie \
    -fno-omit-frame-pointer \
    -nostdlibinc \
    -mcmodel=kernel \
    -mno-red-zone \
    -mno-80387 -mno-mmx -mno-sse -mno-sse2 -msoft-float \
    -Ikernel \
    -isystem $(LIMINE_PROTOCOL_DIR)/include \
    -MMD -MP \
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
    -MMD -MP
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

$(OBJDIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS    $<"
	@$(CXX) $(ASFLAGS) -MJ $@.json -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	@echo "  LD    $@"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@
	@# compile_commands.json for CLion / clangd
	@{ echo '['; find $(OBJDIR) -name '*.o.json' -exec cat {} + | sed '$$ s/,$$//'; echo ']'; } > compile_commands.json

# ---------------------------------------------------------------------------
# Initramfs: the root filesystem the kernel unpacks at boot (cpio, newc)
# ---------------------------------------------------------------------------
$(TESTS_DIR)/%: userspace/%.c
	@mkdir -p $(dir $@)
	@echo "  USER  $<"
	@$(USER_CC) $(USER_CFLAGS) $< -o $@ -lm

$(TESTS_DIR)/%: userspace/ia32/%.c
	@mkdir -p $(dir $@)
	@echo "  USER32 $<"
	@$(USER_CC) $(USER32_CFLAGS) $< -o $@

$(DEBIAN_STAMP): tools/debian-fetch.py Makefile
	@echo "  DEB   $(DEBIAN_PKGS) (+ dependencies, Debian $(DEBIAN_SUITE))"
	@python3 tools/debian-fetch.py --suite $(DEBIAN_SUITE) --out $(DEBIAN_ROOT) --cache deps/debian --exclude $(DEBIAN_EXCLUDE) --repo "$(TRINITY_REPO)" $(DEBIAN_PKGS) $(TRINITY_PKGS)
	@touch $@

$(DEBIAN_I386_STAMP): tools/debian-fetch.py Makefile
	@echo "  DEB   $(DEBIAN_I386_PKGS) (i386, Debian $(DEBIAN_SUITE))"
	@python3 tools/debian-fetch.py --arch i386 --suite $(DEBIAN_SUITE) --out $(DEBIAN_I386_ROOT) --cache deps/debian --exclude $(DEBIAN_I386_EXCLUDE) $(DEBIAN_I386_PKGS)
	@touch $@

$(INITRAMFS): $(KERNEL) tools/mkfontdir.py $(DEBIAN_STAMP) $(DEBIAN_I386_STAMP) $(USER_PROGS) $(USER32_PROGS) $(shell find rootfs -type f 2>/dev/null) $(shell find rootfs -type d 2>/dev/null) Makefile
	@echo "  ROOT  $(ROOTFS)"
	@rm -rf $(ROOTFS)
	@cp -a $(DEBIAN_ROOT) $(ROOTFS)
	@rm -f $(ROOTFS)/.fetched
	@# busybox provides the basic commands (ls, cat, grep, ...) that coreutils would
	@for applet in $$($(DEBIAN_ROOT)/usr/bin/busybox --list); do \
	    [ -e $(ROOTFS)/usr/bin/$$applet ] || [ -e $(ROOTFS)/usr/sbin/$$applet ] || \
	        ln -s busybox $(ROOTFS)/usr/bin/$$applet; done
	@ln -sf busybox $(ROOTFS)/usr/bin/sh
	@mkdir -p $(ROOTFS)/opt/syluxium/tests
	@cp $(USER_PROGS) $(USER32_PROGS) $(ROOTFS)/opt/syluxium/tests/
	@# 32-bit Debian userland (multiarch, as on Debian): libraries in
	@# /usr/lib/i386-linux-gnu, ld-linux.so.2, programs installed as <name>-i386
	@mkdir -p $(ROOTFS)/usr/lib/i386-linux-gnu
	@cp -a $(DEBIAN_I386_ROOT)/usr/lib/i386-linux-gnu/. $(ROOTFS)/usr/lib/i386-linux-gnu/
	@rm -rf $(ROOTFS)/usr/lib/i386-linux-gnu/gconv   # iconv modules: 8 MiB nobody needs here
	@cp -a $(DEBIAN_I386_ROOT)/usr/lib/ld-linux.so.2 $(ROOTFS)/usr/lib/
	@for prog in $(DEBIAN_I386_PROGS); do cp $(DEBIAN_I386_ROOT)/usr/bin/$$prog $(ROOTFS)/usr/bin/$$prog-i386; done
	@cp -r rootfs/. $(ROOTFS)/
	@# What the installer puts on the new disk: kernel, Limine (UEFI + BIOS stages), its installer.
	@mkdir -p $(ROOTFS)/usr/lib/syluxium/boot
	@cp $(KERNEL) $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/limine-bios.sys $(ROOTFS)/usr/lib/syluxium/boot/
	@$(USER_CC) -static -O2 -w -o $(ROOTFS)/usr/lib/syluxium/boot/limine $(LIMINE_DIR)/limine.c
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
# Bootable MBR disk image: one FAT32 EFI system partition, BIOS + UEFI boot.
# Built entirely without root using sfdisk + mtools.
# ---------------------------------------------------------------------------
$(IMAGE): $(KERNEL) $(INITRAMFS) limine.conf $(LIMINE_DIR)/limine
	@echo "  IMG   $@"
	@# Sized to fit: kernel + initramfs + 32 MiB headroom, at least 128 MiB.
	@# Built as $@.tmp and renamed only on success, so a failed build never
	@# leaves a broken image that make would consider up to date.
	@set -e; tmp=$@.tmp; rm -f $$tmp; \
	 mb=$$(( ($$(stat -c %s $(KERNEL)) + $$(stat -c %s $(INITRAMFS))) / 1048576 + 32 )); \
	 [ $$mb -lt 128 ] && mb=128; \
	 dd if=/dev/zero of=$$tmp bs=1M count=$$mb status=none; \
	 echo 'label: dos' | sfdisk --quiet $$tmp; \
	 echo 'start=2048, type=ef, bootable' | sfdisk --quiet --append $$tmp; \
	 mformat -i $$tmp@@1M -F -v SYLUXIUM ::; \
	 mmd   -i $$tmp@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine; \
	 mcopy -i $$tmp@@1M $(KERNEL) ::/boot/syluxium.elf; \
	 mcopy -i $$tmp@@1M $(INITRAMFS) ::/boot/initramfs.cpio; \
	 mcopy -i $$tmp@@1M limine.conf $(LIMINE_DIR)/limine-bios.sys ::/boot/limine; \
	 mcopy -i $$tmp@@1M $(LIMINE_DIR)/BOOTX64.EFI ::/EFI/BOOT; \
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
