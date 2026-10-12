#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# scripts/format.sh [DIR]         -> format sources (default: checkout)
# scripts/format.sh --check [DIR] -> list mismatches; exit 1 if any
# Requires clang-format 23 + uvx/ruff (pinned below); always uses checkout styles.
# Scope: C/C++ in runtime/, tools/; Python <=2 levels deep + .claude/hooks/.
# Excludes third-party trees, overlay_font.h + glfn.h (unstable X-macro formatting).
set -euo pipefail

CLANG_FORMAT_MAJOR=23
RUFF=ruff@0.17.0

repo=$(cd "$(dirname "$0")/.." && pwd)
check=0
root=$repo
for arg in "$@"; do
    case $arg in
        --check) check=1 ;;
        -*) echo "Usage: $0 [--check] [DIR]" >&2; exit 2 ;;
        *) root=$(cd "$arg" && pwd) ;;
    esac
done

if ! command -v clang-format >/dev/null; then
    echo "clang-format $CLANG_FORMAT_MAJOR not on PATH" >&2
    exit 1
fi
version=$(clang-format --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p')
if [ "$version" != "$CLANG_FORMAT_MAJOR" ]; then
    echo "clang-format: need $CLANG_FORMAT_MAJOR, found ${version:-unknown}" >&2
    exit 1
fi

cd "$root"
# Upstream imports may lack tools/ + .claude/hooks/.
c_dirs=()
for d in runtime tools; do [ -d "$d" ] && c_dirs+=("$d"); done
py_extra=()
[ -d .claude/hooks ] && py_extra+=(.claude/hooks)
c_files=()
if [ ${#c_dirs[@]} -gt 0 ]; then
    mapfile -t c_files < <(find "${c_dirs[@]}" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
        -not -name 'overlay_font.h' -not -name 'glfn.h' | sort)
fi
mapfile -t py_files < <( (find . -maxdepth 2 -name '*.py' -not -path './third_party/*' -not -path './.cache/*' \
    -not -path './build*' -not -path './python/*'; [ ${#py_extra[@]} -eq 0 ] || find "${py_extra[@]}" -name '*.py') | sort)

# Portable change detector; FreeBSD lacks sha1sum.
digest() { cat "${c_files[@]}" | cksum; }

status=0
if [ $check = 1 ]; then
    for f in "${c_files[@]}"; do
        clang-format --style="file:$repo/.clang-format" --dry-run -Werror "$f" 2>/dev/null || { echo "$f"; status=1; }
    done
    if [ ${#py_files[@]} -gt 0 ]; then
        uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" --check "${py_files[@]}" || status=1
    fi
    [ $status = 0 ] || echo "Formatting differs; run scripts/format.sh \"$root\"" >&2
else
    # Repeat until stable: trailing comments may realign on pass 2.
    # Skip empty input to avoid stdin reads or whole-tree formatting.
    if [ ${#c_files[@]} -gt 0 ]; then
        for pass in 1 2 3 4 5; do
            before=$(digest)
            clang-format --style="file:$repo/.clang-format" -i "${c_files[@]}"
            [ "$(digest)" = "$before" ] && break
            if [ $pass = 5 ]; then
                echo "clang-format unstable after 5 passes" >&2
                exit 1
            fi
        done
    fi
    if [ ${#py_files[@]} -gt 0 ]; then
        uvx -q "$RUFF" format --no-cache --config "$repo/pyproject.toml" -q "${py_files[@]}"
    fi
fi
exit $status
