#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# scripts/make-appimage.sh -> dist/ScarletNX-<arch>.AppImage
# Requires Arch/pkgforge, quick-sharun on PATH, runtime installed into /usr.
# Bundles runtime deps, SDL3, Vk loader + Mesa. CI: .github/workflows/build.yml.
# Packaging: https://github.com/pkgforge-dev/Anylinux-AppImages
# $APPIMAGE selects the game: games/Game.AppImage -> games/Game_code.so.
set -euo pipefail

ARCH=$(uname -m)
REPOSITORY=${GITHUB_REPOSITORY:-fuddlesworth/SwitchRecomp}
VERSION=${VERSION:-$(git -C "$(dirname "$0")/.." describe --always --dirty 2>/dev/null || echo unknown)}
export ARCH VERSION
export OUTPATH=./dist
export OUTNAME=ScarletNX-$ARCH.AppImage
export ICON=/usr/share/icons/hicolor/scalable/apps/wimsy.svg
export DESKTOP=/usr/share/applications/scarletnx.desktop
# AppImageUpdate: rolling nightly .zsync.
export UPINFO="gh-releases-zsync|${REPOSITORY%/*}|${REPOSITORY#*/}|nightly|*$ARCH.AppImage.zsync"
# No game module during packaging: declare dlopen deps instead of tracing exec.
export STRACE_MODE=0 DEPLOY_SDL=1 DEPLOY_VULKAN=1

quick-sharun /usr/bin/scarletnx

# Bundle installed licenses + Arch package license/version inventory.
appdir=${APPDIR:-$PWD/AppDir}
if [ ! -d "$appdir/lib" ]; then
    echo "missing $appdir/lib; check quick-sharun layout" >&2
    exit 1
fi
mkdir -p "$appdir/share/licenses"
cp -r /usr/share/licenses/scarletnx "$appdir/share/licenses/"
mapfile -t packages < <(cd "$appdir/lib" && find . -name '*.so*' -type f | while read -r lib; do
    pacman -Qqo "/usr/lib/${lib#./}" 2>/dev/null || true
done | sort -u)
for pkg in "${packages[@]}"; do
    pacman -Qi "$pkg" | grep -E '^(Name|Version|Licenses) '
    echo
    if [ -d "/usr/share/licenses/$pkg" ]; then
        cp -r "/usr/share/licenses/$pkg" "$appdir/share/licenses/"
    fi
done >"$appdir/share/licenses/bundled-packages.txt"

quick-sharun --make-appimage

# Probe missing-module diagnostics: $APPIMAGE naming + --game override.
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
cp "$OUTPATH/$OUTNAME" "$probe/Probe.AppImage"
check() { # Expected module path, then AppImage args
    local want=$1 out
    shift
    out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 ./Probe.AppImage "$@" 2>&1 || true)
    if ! grep -qF "$want" <<<"$out"; then
        echo "AppImage module lookup mismatch; expected: $want" >&2
        echo "$out" >&2
        exit 1
    fi
}
check "$probe/Probe_code.so"
check "$probe/games/Other_code.so" --game="$probe/games/Other"
echo "AppImage: $OUTPATH/$OUTNAME (name + --game lookup passed)"
