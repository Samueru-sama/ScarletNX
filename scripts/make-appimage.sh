#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Makes dist/ScarletNX-<arch>.AppImage, a runtime that runs on any Linux, with pkgforge's quick-sharun
# (https://github.com/pkgforge-dev/Anylinux-AppImages): it bundles the runtime with every library it loads, SDL3, the
# Vulkan loader and Mesa's Vulkan drivers included. CI runs it in pkgforge's Arch container after `cmake --install`
# into /usr (.github/workflows/build.yml), and quick-sharun has to be on PATH (pkgforge-dev/anylinux-setup-action puts
# it there).
#
# A copy named after a game works like the bare runtime: games/Catherine.AppImage (or games/Catherine) loads
# games/Catherine_code.so, since the runtime takes its name and folder from $APPIMAGE.
#
# With SYSROOT set, the AppImage is made for another architecture from this machine: the runtime was cross-compiled
# into the sysroot, and the libraries and licenses to bundle come from there (CI makes the aarch64, loongarch64,
# ppc64le and riscv64 ones this way, .github/workflows/build.yml). The packaging tools all run natively; only the
# loader behind `ldd` and the smoke test below run as target programs, through qemu-user.
set -euo pipefail

SYSROOT=${SYSROOT:-} # the target's system root, empty to package this machine's own /usr
ARCH=${ARCH:-$(uname -m)}
REPOSITORY=${GITHUB_REPOSITORY:-scarletnx/ScarletNX}
VERSION=${VERSION:-$(git -C "$(dirname "$0")/.." describe --always --dirty 2>/dev/null || echo unknown)}
export ARCH VERSION SYSROOT
# The architecture of the runtime inside the AppImage: appimagetool names it in the metadata and picks its runtime
# (uruntime) by it, and quick-sharun names its downloads by it (from `uname -m`, see the shims below)
export APPIMAGE_ARCH=$ARCH
export OUTPATH=./dist
export OUTNAME=ScarletNX-$ARCH.AppImage
export ICON=/usr/share/icons/hicolor/scalable/apps/wimsy.svg
export DESKTOP=/usr/share/applications/scarletnx.desktop
# AppImageUpdate finds new builds in the rolling release through the .zsync file next to the AppImage
export UPINFO="gh-releases-zsync|${REPOSITORY%/*}|${REPOSITORY#*/}|nightly|*$ARCH.AppImage.zsync"
# The runtime exits at once without a game module, so tracing it for dlopened libraries finds nothing: name them
export STRACE_MODE=0 DEPLOY_SDL=1 DEPLOY_VULKAN=1

# `pacman` of the build machine, over the sysroot's package database when there is one: the packages the bundled
# libraries came from are recorded there, under the target's own paths
pacman_query() {
    if [ -n "$SYSROOT" ]; then
        pacman --dbpath "$SYSROOT/var/lib/pacman" "$@"
    else
        pacman "$@"
    fi
}

binary=/usr/bin/scarletnx
runner= # a cross-built AppImage is a target program: run it through qemu-user
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -n "$SYSROOT" ]; then
    # The runtime, its desktop entry and its icon were installed into the sysroot
    binary=$SYSROOT$binary
    ICON=$SYSROOT$ICON
    DESKTOP=$SYSROOT$DESKTOP
    export LIB_DIR=$SYSROOT/usr/lib
    runner=qemu-$ARCH-static

    # quick-sharun takes the architecture of everything it bundles from `uname -m`, and the libraries a binary loads
    # from `ldd`. The build machine's ldd refuses the target's binaries: it runs the build machine's loader over them
    # (the kernel's binfmt handlers would not help either, ldd runs its loader directly). Answer both for the target
    shim=$tmp/shim
    mkdir -p "$shim"
    cat >"$shim/uname" <<EOF
#!/bin/sh
[ "\${1:-}" = -m ] && echo $ARCH && exit 0
exec /usr/bin/uname "\$@"
EOF
    interp=$(patchelf --print-interpreter "$binary")
    cat >"$shim/ldd" <<EOF
#!/bin/sh
# The sysroot's loader lists the same libraries ldd would, with the paths it found them at in the sysroot
out=\$(qemu-$ARCH-static -L "$SYSROOT" "$SYSROOT$interp" --list "\$@" 2>/dev/null) || exec /usr/bin/ldd "\$@"
printf '%s\n' "\$out"
EOF
    chmod +x "$shim/uname" "$shim/ldd"
    PATH=$shim:$PATH
    export PATH
fi

# hactool too: --extract looks for it next to the runtime, and without it tries to build it from a checkout
quick-sharun "$binary" "$SYSROOT/usr/bin/hactool"

# The licenses travel with the AppImage: the runtime's and those of the libraries built into it (`cmake --install` put
# them in the licenses directory of the system), and those of the Arch packages quick-sharun took the bundled libraries
# from, with a list of those packages, their versions and licenses
appdir=${APPDIR:-$PWD/AppDir}
if [ ! -d "$appdir/lib" ]; then
    echo "quick-sharun left no $appdir/lib: where it puts the bundled libraries has changed" >&2
    exit 1
fi
mkdir -p "$appdir/share/licenses"
cp -r "$SYSROOT/usr/share/licenses/scarletnx" "$appdir/share/licenses/"
mapfile -t packages < <(cd "$appdir/lib" && find . -name '*.so*' -type f | while read -r lib; do
    # Both spellings of the path: a cross build's database lists the target's own, without the sysroot in front
    pacman_query -Qqo "/usr/lib/${lib#./}" 2>/dev/null ||
        pacman_query -Qqo "$SYSROOT/usr/lib/${lib#./}" 2>/dev/null || true
done | sort -u)
for pkg in "${packages[@]}"; do
    pacman_query -Qi "$pkg" | grep -E '^(Name|Version|Licenses) '
    echo
    if [ -d "$SYSROOT/usr/share/licenses/$pkg" ]; then
        cp -r "$SYSROOT/usr/share/licenses/$pkg" "$appdir/share/licenses/"
    fi
done >"$appdir/share/licenses/bundled-packages.txt"

quick-sharun --make-appimage

# Without a game the runtime says which module it looked for. Under another name, it must look next to the AppImage
# for that name's module, which shows the libraries load and the name and folder come from $APPIMAGE. With --game it
# must look for the module that names
probe=$tmp/probe
mkdir -p "$probe"
cp "$OUTPATH/$OUTNAME" "$probe/Probe.AppImage"
check() { # expected module, then the AppImage's arguments
    local want=$1 out
    shift
    out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 $runner ./Probe.AppImage "$@" 2>&1 || true)
    if ! grep -qF "$want" <<<"$out"; then
        echo "The AppImage did not look for $want. It printed:" >&2
        echo "$out" >&2
        exit 1
    fi
}
check "$probe/Probe_code.so"
check "$probe/games/Other_code.so" --game="$probe/games/Other"
echo "AppImage checked: it looks for its game module next to itself, or where --game says"
