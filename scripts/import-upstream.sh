#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# scripts/import-upstream.sh RELEASE.zip VERSION
# -> upstream-VERSION: raw import; upstream: formatted commit.
# Current branch/index/worktree unchanged. Port to main manually.
# Excludes bundled toolchains, binaries, keys + game data.
# Release-to-release diff: upstream~2..upstream.
set -euo pipefail
# Abort snapshot() on failed git add, including inside $(...).
shopt -s inherit_errexit

if [ $# -ne 2 ]; then
    echo "Usage: $0 RELEASE.zip VERSION" >&2
    exit 2
fi
zip=$1
version=$2
repo=$(git rev-parse --show-toplevel)

if git -C "$repo" rev-parse -q --verify "refs/tags/upstream-$version" >/dev/null; then
    echo "tag exists: upstream-$version" >&2
    exit 1
fi
# Do not move HEAD beneath a checked-out worktree.
if git -C "$repo" worktree list --porcelain | grep -qx 'branch refs/heads/upstream'; then
    echo "upstream is checked out; switch that worktree away (git worktree list)" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
unzip -q "$zip" -d "$tmp/x"
src="$tmp/x/SwitchRecomp"
if [ ! -d "$src/runtime" ]; then
    echo "$zip: missing SwitchRecomp/runtime/" >&2
    exit 1
fi

# Private index; -f bypasses global ignores. Exclude binaries/game data explicitly.
export GIT_INDEX_FILE="$tmp/index"
snapshot() {
    rm -f "$GIT_INDEX_FILE"
    git -C "$repo" --work-tree="$src" add -A -f -- . \
        ':(exclude)python' ':(exclude)tcc' ':(exclude)gcc' ':(exclude)w64devkit' ':(exclude)bin' ':(exclude)*.exe' \
        ':(exclude)*.a' ':(exclude)*.lib' ':(exclude)*.dll' ':(exclude)*.so' \
        ':(exclude)*.keys' ':(exclude)*.xci' ':(exclude)*.nsp' ':(exclude)*.nsz' ':(exclude)*.xcz' ':(exclude)*.nca' \
        ':(exclude)*.nro' ':(exclude)*.nso' ':(exclude)*.tik' ':(exclude)*.cert'
    local tree
    tree=$(git -C "$repo" write-tree)
    if [ -z "$(git -C "$repo" ls-tree "$tree" runtime)" ]; then
        echo "import missing runtime/" >&2
        exit 1
    fi
    echo "$tree"
}

parent=$(git -C "$repo" rev-parse -q --verify refs/heads/upstream || true)
raw_tree=$(snapshot)
raw=$(git -C "$repo" commit-tree "$raw_tree" ${parent:+-p "$parent"} \
    -m "Import SwitchRecomp $version" \
    -m "MZ Labs $(basename "$zip"); excludes bundled toolchains, binaries and game data.")

"$repo/scripts/format.sh" "$src"
formatted_tree=$(snapshot)
formatted=$(git -C "$repo" commit-tree "$formatted_tree" -p "$raw" \
    -m "Format SwitchRecomp $version" \
    -m "scripts/format.sh; matches main for release-to-release diffs.")

git -C "$repo" update-ref refs/heads/upstream "$formatted" ${parent:+"$parent"}
git -C "$repo" tag "upstream-$version" "$raw"
echo "upstream: $formatted; raw import: upstream-$version"
