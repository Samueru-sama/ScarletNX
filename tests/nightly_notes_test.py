#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/nightly-notes.py on a history of its own: merged pull requests, direct commits, a branch never merged"""

DOWNLOAD = 'https://github.com/owner/nightly/releases/download/t'


import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location(
    'nightly_notes', Path(__file__).resolve().parent.parent / 'scripts' / 'nightly-notes.py'
)
nightly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nightly)
failures = []


def check(what, ok):
    if not ok:
        failures.append(what)


with tempfile.TemporaryDirectory() as repo:
    # written on the 1st, landed on main on the 15th: the title dates the landing
    env = dict(os.environ, GIT_COMMITTER_DATE='2026-10-15T12:00:00Z', GIT_AUTHOR_DATE='2026-10-01T12:00:00Z')

    def git(*args):
        return subprocess.run(
            [
                'git',
                '-C',
                repo,
                '-c',
                'user.name=t',
                '-c',
                'user.email=t@t',
                '-c',
                'commit.gpgsign=false',
                '-c',
                'core.hooksPath=' + os.devnull,
                *args,
            ],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        ).stdout.strip()

    git('init', '-q', '-b', 'main')
    git('commit', '-q', '--allow-empty', '-m', 'the previous nightly')
    previous = git('rev-parse', 'HEAD')
    git('switch', '-q', '-c', 'feature')
    git('commit', '-q', '--allow-empty', '-m', '[cpu] a commit of the pull request')
    git('switch', '-q', 'main')
    git('merge', '-q', '--no-ff', 'feature', '-m', 'Merge pull request #5 from someone/feature\n\n[ci] its title')
    git('commit', '-q', '--allow-empty', '-m', '[docs] a commit pushed to main')
    sha = git('rev-parse', 'HEAD')
    git('switch', '-q', '-c', 'elsewhere', previous)
    git('commit', '-q', '--allow-empty', '-m', 'never merged')
    unmerged = git('rev-parse', 'HEAD')

    check(
        'changes, newest first, a pull request by its title',
        nightly.changes(repo, previous, sha) == ['[docs] a commit pushed to main', '[ci] its title (#5)'],
    )
    check('no changes without a previous nightly', nightly.changes(repo, '', sha) is None)
    check('no changes from a commit that is not an ancestor', nightly.changes(repo, unmerged, sha) is None)
    check('the title', nightly.title(repo, sha) == f'Nightly 2026-10-15 ({sha[:7]})')

    files = ['ScarletNX-android-universal.apk', 'ScarletNX-linux-aarch64.AppImage', 'ScarletNX-linux-x86_64.AppImage']
    text = nightly.notes(repo, sha, previous, 'owner/repo', files, DOWNLOAD, 'Kept.')
    check(
        'the built commit is the first one the notes name, where the next nightly looks',
        re.search(r'[0-9a-f]{40}', text).group(0) == sha,
    )
    rows = [line.split('[')[1].split(']')[0] for line in text.splitlines() if line.startswith('| [')]
    check(
        'the files in the table order',
        rows
        == ['ScarletNX-linux-x86_64.AppImage', 'ScarletNX-linux-aarch64.AppImage', 'ScarletNX-android-universal.apk'],
    )
    try:
        nightly.notes(repo, sha, previous, 'owner/repo', files + ['ScarletNX-windows-arm64.zip'], DOWNLOAD, 'Kept.')
        check('a file with no system stops the notes', False)
    except SystemExit as stop:
        check(
            'a file with no system stops the notes, saying where to add it',
            'ScarletNX-windows-arm64.zip' in str(stop) and 'SYSTEMS in scripts/nightly-notes.py' in str(stop),
        )
    check('a merged pull request listed', '- [ci] its title (#5)' in text)
    check('the source named', f'https://github.com/owner/repo/tree/{sha}' in text)
    check(
        'no changes section without a previous nightly',
        '### Changes' not in nightly.notes(repo, sha, '', 'owner/repo', files, DOWNLOAD, 'Kept.'),
    )
    check(
        'the same commit built again',
        'None: the same commit' in nightly.notes(repo, sha, sha, 'owner/repo', files, DOWNLOAD, 'Kept.'),
    )
    check(
        'the files linked from the release they are in, and what becomes of it',
        f'[ScarletNX-android-universal.apk]({DOWNLOAD}/ScarletNX-android-universal.apk)' in text
        and 'committed 2026-10-15. Kept.' in text,
    )

    # two more than the list holds, on top of the two above
    git('switch', '-q', 'main')
    for n in range(nightly.MOST_CHANGES + 2):
        git('commit', '-q', '--allow-empty', '-m', f'commit {n}')
    many = nightly.notes(repo, git('rev-parse', 'HEAD'), previous, 'owner/repo', files, DOWNLOAD, 'Kept.')
    listed = many.split('### Changes')[1].split('### Source')[0].count('\n- ')
    check('a long list cut, the rest counted', listed == nightly.MOST_CHANGES + 1 and '- and 4 more' in many)

for failure in failures:
    print('FAIL:', failure)
print('nightly-notes:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
