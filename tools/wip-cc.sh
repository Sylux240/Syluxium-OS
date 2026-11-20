#!/bin/sh
# Syntax-check Linux sources in wip/ with the LinuxKPI flags (Makefile's
# LKPI_LINUX_CFLAGS) without adding them to the kernel build.
#   tools/wip-cc.sh wip/gpu/dma-buf/dma-fence.c [...]
CFLAGS="--target=x86_64-unknown-none-elf -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
 -Wno-missing-field-initializers -Wno-pointer-sign -ffreestanding -fno-pic -fno-pie -nostdlibinc -mcmodel=kernel \
 -mno-red-zone -mno-sse -msoft-float"
LK="-Ikernel/linuxkpi/include -include linux/kconfig.h -Werror=implicit-function-declaration -Werror=int-conversion \
 -Werror=incompatible-pointer-types -D__KERNEL__ -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
 -Wno-gnu-variable-sized-type-not-at-end -Wno-initializer-overrides -Wno-unused-const-variable -Wno-implicit-fallthrough \
 -Wno-missing-field-initializers -Wno-address-of-packed-member -Wno-default-const-init-field-unsafe -Wno-unknown-warning-option"
for f in "$@"; do
    clang $CFLAGS $LK -DKBUILD_MODNAME='"drm"' -DKBUILD_BASENAME='"x"' -fsyntax-only -ferror-limit=0 "$f" 2>&1
done
