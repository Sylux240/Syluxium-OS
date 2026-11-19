#!/bin/sh
# Finish the Wine installation in the image's root file system (what Debian's
# package scripts and update-alternatives would do, plus Syluxium's tweaks).
#
#   tools/wine-setup.sh ROOTFS WINE_I386_ROOT
#
#  - 32-bit Windows DLLs from Debian's i386 libwine go next to the 64-bit
#    ones (/usr/lib/x86_64-linux-gnu/wine/i386-windows): the 64-bit Wine
#    then runs 32-bit programs in WoW64 mode. That replaces the wine32
#    package and its whole i386 Linux library stack.
#  - zlib1.dll, made a Wine builtin from libz-mingw-w64 (libwine's postinst).
#  - Debian ships the Windows DLLs with DWARF debug info: 1.3 GB; stripped
#    they're 480 MB.
#  - wine-stable/winecfg/... names (update-alternatives).
#  - TDE file types: .exe/.msi open with Wine (double-click).
#  - A template Wine prefix for /usr/bin/wine to copy on first use, made by
#    running the image's own Wine in a sandbox (bwrap). Without bwrap the
#    image just has no template and Wine creates prefixes itself.
set -e
ROOTFS=$1
I386=$2
WINE=$ROOTFS/usr/lib/x86_64-linux-gnu/wine

[ -d "$WINE/x86_64-windows" ] || { echo "  wine: not in the image, skipped"; exit 0; }

rm -rf "$WINE/i386-windows"
cp -a "$I386/usr/lib/i386-linux-gnu/wine/i386-windows" "$WINE/i386-windows"

for mingw_arch in i686 x86_64; do
    wine_arch=$mingw_arch
    [ $mingw_arch = i686 ] && wine_arch=i386
    in=$ROOTFS/usr/$mingw_arch-w64-mingw32/lib/zlib1.dll
    out=$WINE/$wine_arch-windows/zlib1.dll
    { head -c 64 "$in"; printf 'Wine builtin DLL'; printf '\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0'; tail -c +97 "$in"; } > "$out"
done

if command -v llvm-strip > /dev/null; then
    find "$WINE/x86_64-windows" "$WINE/i386-windows" -type f -print0 | xargs -0 -P "$(nproc)" -n 64 llvm-strip --strip-debug
else
    echo "  warning: llvm-strip not found: Wine's DLLs keep their debug info (+800 MB)"
fi

# update-alternatives: the plain names point at Debian's *-stable programs.
for prog in msiexec regedit regsvr32 wineboot winecfg wineconsole winedbg winefile winepath wineserver; do
    ln -sf $prog-stable "$ROOTFS/usr/bin/$prog"
done
ln -sf wine "$ROOTFS/usr/bin/wine-stable"

# The desktop (TDE) file types: .exe files get Wine's icon, open with Wine
# (/usr/share/applications/wine.desktop) and are no longer "also" Linux
# executables, which Konqueror would try to run directly when they have
# the x bit (as everything on a FAT stick does). .msi files get a type.
MIMELNK=$ROOTFS/opt/trinity/share/mimelnk/application
if [ -f "$MIMELNK/x-msdos-program.desktop" ]; then
    sed -i -e '/^X-TDE-IsAlso=/d' -e 's/^Icon=.*/Icon=exec_wine/' \
        -e 's/^MimeType=.*/&\nPatterns=*.exe;*.EXE;/' "$MIMELNK/x-msdos-program.desktop"
    printf '[Desktop Entry]\nComment=Windows Installer Package\nIcon=exec_wine\nType=MimeType\nMimeType=application/x-msi\nPatterns=*.msi;*.MSI;\n' \
        > "$MIMELNK/x-msi.desktop"
fi

# The template prefix.
TEMPLATE=/usr/lib/syluxium/wine-prefix
rm -rf "$ROOTFS$TEMPLATE"
if ! command -v bwrap > /dev/null; then
    echo "  warning: bwrap not found: no template Wine prefix (the first 'wine' run creates one, ~500 MB)"
    exit 0
fi
mkdir -p "$ROOTFS$TEMPLATE"
sandbox() {
    bwrap --bind "$ROOTFS" / --dev /dev --proc /proc --tmpfs /tmp --dir /tmp/home \
        --clearenv --setenv PATH /usr/bin:/bin --setenv HOME /tmp/home --setenv USER syluxium \
        --setenv LOGNAME syluxium --setenv XDG_RUNTIME_DIR /tmp --setenv WINEPREFIX $TEMPLATE --setenv WINEDEBUG -all \
        --setenv WINEDLLOVERRIDES 'mscoree,mshtml=;winemenubuilder.exe=d' "$@"
}
sandbox /usr/lib/wine/wine64 wineboot.exe --init
# winemenubuilder (menu entries for installed programs) may not take over
# file types such as .txt in the desktop's associations.
sandbox /usr/lib/wine/wine64 reg.exe add 'HKCU\Software\Wine\FileOpenAssociations' /v Enable /d N /f > /dev/null
sandbox /usr/lib/wine/wineserver64 -w
rm -f "$ROOTFS$TEMPLATE/wineserver" # the build-time server's info file (mode 0400)
python3 tools/wine-prefix-links.py "$ROOTFS" "$ROOTFS$TEMPLATE"
