#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Write the nightly release's title and notes: which file is for which system, what changed since the previous
nightly, and where the source is, which the GPL asks for beside the binaries.

    scripts/nightly-notes.py SHA [PREVIOUS] --repo OWNER/REPO --files NAME... [--checkout DIR]
    scripts/nightly-notes.py SHA --title [--checkout DIR]

SHA is the commit built and PREVIOUS the one the nightly it replaces was built from: the changes come from the
checkout's history between them, and are left out without PREVIOUS or when it is not an ancestor of SHA. The notes
link SHA first, which is where the next nightly finds it. .github/workflows/build.yml's release job runs this.
"""

import argparse
import re
import subprocess

SERVER = 'https://github.com'
# The release's files in the order the table lists them, with what each is for. A file the release job uploads that
# is missing here stops the notes (the files are out by then): add its line
SYSTEMS = {
    'ScarletNX-x86_64.AppImage': 'Linux, x86-64',
    'ScarletNX-aarch64.AppImage': 'Linux, ARM64',
    'ScarletNX-windows-x86_64.zip': 'Windows, x86-64',
    'ScarletNX-android.apk': 'Android 5.0 and later, ARM64 and x86-64 (signed with a debug key)',
}
MERGE = re.compile(r'Merge pull request #(\d+) from \S+')
MOST_CHANGES = 30


def git(checkout, *args):
    return subprocess.run(['git', '-C', checkout, *args], check=True, capture_output=True, text=True).stdout


def changes(checkout, previous, sha):
    """What main got from PREVIOUS to SHA, newest first: a merged pull request by its title, any other commit by its
    subject. None when PREVIOUS is missing or not an ancestor of SHA"""
    if not previous:
        return None
    ancestor = subprocess.run(
        ['git', '-C', checkout, 'merge-base', '--is-ancestor', previous, sha], capture_output=True
    )
    if ancestor.returncode:
        return None
    items = []
    for record in git(checkout, 'log', '--first-parent', '--format=%s%x1f%b%x1e', f'{previous}..{sha}').split('\x1e'):
        record = record.strip('\n')
        if not record:
            continue
        subject, _, body = record.partition('\x1f')
        merge = MERGE.fullmatch(subject)
        if merge:
            title = next((line.strip() for line in body.splitlines() if line.strip()), subject)
            items.append(f'{title} (#{merge.group(1)})')
        else:
            items.append(subject)
    return items


def title(checkout, sha):
    return f'Nightly {git(checkout, "log", "-1", "--format=%cs", sha).strip()} ({sha[:7]})'


def notes(checkout, sha, previous, repo, files):
    url = f'{SERVER}/{repo}'
    date = git(checkout, 'log', '-1', '--format=%cs', sha).strip()
    lines = [
        f'Built from [`{sha[:7]}`]({url}/commit/{sha}), committed {date}. Each push to main replaces it.',
        '',
        '| File | For |',
        '|---|---|',
    ]
    unknown = [name for name in files if name not in SYSTEMS]
    if unknown:
        raise SystemExit(f'{", ".join(unknown)}: what system is it for? Add it to SYSTEMS in scripts/nightly-notes.py')
    for name in sorted(files, key=list(SYSTEMS).index):
        lines.append(f'| [{name}]({url}/releases/download/nightly/{name}) | {SYSTEMS[name]} |')
    lines += [
        '',
        f'How to run them: [USAGE.md]({url}/blob/{sha}/docs/USAGE.md), and '
        f'[ANDROID.md]({url}/blob/{sha}/docs/ANDROID.md) for the app.',
    ]
    items = changes(checkout, previous, sha)
    if items is not None:
        lines += [
            '',
            f'### Changes since the previous nightly ([`{previous[:7]}`]({url}/compare/{previous}...{sha}))',
            '',
        ]
        lines += [f'- {item}' for item in items[:MOST_CHANGES]] or ['None: the same commit, built again.']
        if len(items) > MOST_CHANGES:
            lines.append(f'- and {len(items) - MOST_CHANGES} more')
    lines += [
        '',
        '### Source and licenses',
        '',
        f'The source of this build is at {url}/tree/{sha}.',
        '',
        '- The third-party libraries built into the runtime come from the release archives that '
        'cmake/dependencies.cmake lists, each checked against the SHA-256 written beside it.',
        "- The AppImages also bundle libraries from Arch Linux packages (Arch Linux ARM's for ARM64): their "
        'licenses are in share/licenses, and the packages in share/licenses/bundled-packages.txt.',
        '- The Windows zip holds the runtime DLLs it needs with their licenses in share/licenses, and the MSYS2 '
        'packages it was built with in share/licenses/msys2/build-packages.txt.',
    ]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('sha')
    parser.add_argument('previous', nargs='?', default='')
    parser.add_argument('--repo')
    parser.add_argument('--files', nargs='+', default=[])
    parser.add_argument('--checkout', default='.')
    parser.add_argument('--title', action='store_true', help="print the release's title only")
    args = parser.parse_args()
    if args.title:
        print(title(args.checkout, args.sha))
    else:
        if not args.repo:
            parser.error('--repo is needed for the notes')
        print(notes(args.checkout, args.sha, args.previous, args.repo, args.files), end='')


if __name__ == '__main__':
    main()
