#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Publish a Build run's packages as releases, and say so on Discord.

    scripts/publish-builds.py plan --repo OWNER/REPO --to OWNER/NIGHTLY
    scripts/publish-builds.py publish --repo OWNER/REPO --to OWNER/NIGHTLY --kind nightly|pr|release --run ID
        --sha SHA [--pr N] [--tag TAG] --artifacts DIR [--checkout DIR] [--keep N]

plan follows the event GitHub Actions ran it for (GITHUB_EVENT_NAME, GITHUB_EVENT_PATH), and writes to GITHUB_OUTPUT
the run whose artifacts publish takes (kind, run, sha, short, pr, tag), when there is one to publish:
- a Build run that ended (workflow_run): a push of a version tag (v...) that built everywhere becomes REPO's release of
  that tag, the latest (a prerelease for a tag with a '-', v1.0.0-rc1); a push to main that built everywhere becomes
  NIGHTLY's release nightly-<date>-<commit>, the latest there, only the newest N nightlies staying, and REPO's
  prerelease "nightly" a link to it; a pull request's run becomes NIGHTLY's prerelease pr-<number>, its files and notes
  replaced at each push;
- a pull request closed (pull_request_target): its prerelease and tag are deleted;
- a fork's pull request is published only once someone of the team (write, maintain or admin) labels it LABEL after
  its last push, which a new push takes off (synchronize). The label put on a finished run publishes it at once.
Nothing is built: the files come from the run's artifacts, which the workflow downloads into DIR between the two (one
folder each, as actions/download-artifact puts them), and no code of the pull request runs. Each file is named
ScarletNX-<system>-<arch> (scripts/nightly-notes.py), an AppImage with the .zsync AppImageUpdate reads beside it.

Discord: each release and each nightly gets a message on DISCORD_WEBHOOK_RELEASES's and DISCORD_WEBHOOK_NIGHTLY's
channels, each pull request one message on DISCORD_WEBHOOK_PRS's, edited at each push and when it closes (its id is
kept in the prerelease's notes). Without a webhook nothing is posted, and the run says so. gh reads and writes REPO
with GH_TOKEN, and NIGHTLY with NIGHTLY_TOKEN. --dry-run reads and prints each write instead.
.github/workflows/publish.yml runs this; tests/publish_builds_test.py runs it against a fake gh and a fake Discord.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('nightly_notes', Path(__file__).with_name('nightly-notes.py'))
nightly_notes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nightly_notes)

SERVER = nightly_notes.SERVER
LABEL = 'publish builds'
TEAM = {'admin', 'maintain', 'write'}
DISCORD = re.compile(r'<!-- discord-message: (\d+) -->')
VERSION = re.compile(r'v\d[0-9A-Za-z.+-]*')
ROLLING = 'nightly'
# A run's artifacts, by the commit's first 7 digits: the release file each gives, ScarletNX-<system>-<arch>
ARTIFACTS = {
    'ScarletNX-linux-x86_64-{short}.AppImage': 'ScarletNX-linux-x86_64.AppImage',
    'ScarletNX-linux-aarch64-{short}.AppImage': 'ScarletNX-linux-aarch64.AppImage',
    'ScarletNX-windows-x86_64-{short}': 'ScarletNX-windows-x86_64.zip',
    'ScarletNX-android-universal-{short}.apk': 'ScarletNX-android-universal.apk',
}
SCARLET = 0xB0102A


class Publisher:
    def __init__(self, args):
        self.repo, self.to, self.dry = args.repo, args.to, args.dry_run
        self.checkout, self.keep = args.checkout, args.keep
        self.artifacts = Path(args.artifacts) if args.artifacts else None
        self.tokens = {self.repo: os.environ.get('GH_TOKEN', ''), self.to: os.environ.get('NIGHTLY_TOKEN', '')}
        self.work = Path(tempfile.mkdtemp(prefix='publish-builds-'))
        self.output = {}

    # ---- GitHub, through gh: REPO with GH_TOKEN, the nightly repository with NIGHTLY_TOKEN
    def token(self, repo):
        if repo == self.to and not self.tokens[repo] and not self.dry:
            raise RuntimeError(f'NIGHTLY_TOKEN is not set: nothing can be written in {self.to}')
        return self.tokens[repo]

    def gh(self, args, repo, data=None, write=False):
        if write and self.dry:
            print('would run: gh', ' '.join(args), json.dumps(data) if data is not None else '')
            return {}
        run = subprocess.run(
            ['gh', *args],
            input=json.dumps(data) if data is not None else None,
            capture_output=True,
            text=True,
            env=dict(os.environ, GH_TOKEN=self.token(repo)),
            check=False,
        )
        if run.returncode:
            raise RuntimeError(f'gh {" ".join(args)}: {run.stderr.strip() or run.stdout.strip()}')
        return json.loads(run.stdout) if run.stdout.strip() else {}

    def read(self, path):
        return self.gh(['api', path], self.repo)

    def pages(self, path, repo):
        """every item of a listing, 100 a page"""
        items, page = [], 1
        while True:
            sep = '&' if '?' in path else '?'
            got = self.gh(['api', f'{path}{sep}per_page=100&page={page}'], repo)
            got = got.get('artifacts', got.get('workflow_runs', got)) if isinstance(got, dict) else got
            items += got
            if len(got) < 100:
                return items
            page += 1

    def write(self, repo, method, path, data=None):
        args = ['api', '-X', method, path, *(['--input', '-'] if data is not None else [])]
        return self.gh(args, repo, data, write=True)

    # ---- releases, of REPO or of the nightly repository
    def releases(self, repo):
        return self.pages(f'repos/{repo}/releases', repo)

    def release(self, repo, tag):
        return next((r for r in self.releases(repo) if r['tag_name'] == tag), None)

    def put_release(self, repo, tag, name, body, prerelease, files, target=None):
        """the release of tag with these notes and exactly these files (each name to its path), made or replaced; it
        and whether it is new"""
        existing = self.release(repo, tag)
        fields = {'name': name, 'body': body, 'prerelease': prerelease, 'make_latest': str(not prerelease).lower()}
        if existing:
            made = self.write(repo, 'PATCH', f'repos/{repo}/releases/{existing["id"]}', fields) or existing
        else:
            fields = dict(fields, tag_name=tag, **({'target_commitish': target} if target else {}))
            made = self.write(repo, 'POST', f'repos/{repo}/releases', fields)
        if files:
            self.gh(
                ['release', 'upload', tag, *map(str, files.values()), '--repo', repo, '--clobber'], repo, write=True
            )
        for asset in (existing or {}).get('assets', []):
            if asset['name'] not in files:  # a file this run did not build: the old one would pass for it
                self.write(repo, 'DELETE', f'repos/{repo}/releases/assets/{asset["id"]}')
        return made, existing is None

    def drop_release(self, repo, release):
        self.write(repo, 'DELETE', f'repos/{repo}/releases/{release["id"]}')
        try:
            self.write(repo, 'DELETE', f'repos/{repo}/git/refs/tags/{release["tag_name"]}')
        except RuntimeError as error:
            if 'Reference does not exist' not in str(error):
                raise
            print(f'{release["tag_name"]} had no tag left')

    # ---- a run's packages
    def packages(self, sha):
        """the release files the run's artifacts give, by name, the files to upload with them (an AppImage's .zsync),
        and the systems it gave none for"""
        files, uploads, missing = {}, {}, []
        for pattern, name in ARTIFACTS.items():
            folder = self.artifacts / pattern.format(short=sha[:7])
            if not folder.is_dir() or not any(folder.iterdir()):
                missing.append(nightly_notes.SYSTEMS[name])
                continue
            path = self.work / name
            if name.endswith('.zip'):  # the Windows build is a folder: zipped as it is
                with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
                    for item in sorted(folder.rglob('*')):
                        if item.is_file():
                            archive.write(item, item.relative_to(folder).as_posix())
            else:
                found = sorted(folder.rglob(name if name.endswith('.AppImage') else '*.apk'))
                if len(found) != 1:
                    raise RuntimeError(f'{folder.name} holds {len(found)} of {name}, not one')
                shutil.copyfile(found[0], path)
            files[name] = uploads[name] = path
            if name.endswith('.AppImage'):
                zsync = folder / f'{name}.zsync'
                if not zsync.is_file():
                    raise RuntimeError(f'{folder.name} has no {zsync.name}, which AppImageUpdate reads')
                uploads[zsync.name] = self.work / zsync.name
                shutil.copyfile(zsync, uploads[zsync.name])
        return files, uploads, missing

    def complete(self, run, sha):
        files, uploads, missing = self.packages(sha)
        if missing:
            raise RuntimeError(f'run {run} built everywhere, yet has no package for {"; ".join(missing)}')
        return files, uploads

    # ---- Discord
    def discord(self, kind, embed, message=None):
        """the message of kind (releases, nightly or prs) posted, or edited when message names it: its id, or None"""
        hook = os.environ.get(f'DISCORD_WEBHOOK_{kind.upper()}', '')
        if not hook:
            print(f'::notice::No DISCORD_WEBHOOK_{kind.upper()}: nothing posted on Discord')
            return message
        body = {'embeds': [embed], 'allowed_mentions': {'parse': []}}
        if self.dry:
            print(f'would {"edit" if message else "post"} on Discord ({kind}):', json.dumps(body))
            return message
        # a webhook of a forum channel or a thread names it in its query (thread_id=...), which both requests keep
        base, _, query = hook.partition('?')
        edit = f'{base}/messages/{message}' + (f'?{query}' if query else '')
        post = f'{base}?' + (f'{query}&' if query else '') + 'wait=true'
        for method, url in (('PATCH', edit), ('POST', post)):
            if method == 'PATCH' and not message:
                continue
            request = urllib.request.Request(
                url,
                data=json.dumps(body).encode(),
                method=method,
                # Discord refuses urllib's own agent
                headers={'Content-Type': 'application/json', 'User-Agent': f'DiscordBot ({SERVER}/{self.repo}, 1)'},
            )
            try:
                with urllib.request.urlopen(request, timeout=30) as reply:
                    return json.loads(reply.read() or b'{}').get('id', message)
            except urllib.error.HTTPError as error:
                if method == 'PATCH' and error.code == 404:  # deleted on Discord: a new one
                    continue
                print(f'::warning::Discord answered {error.code} to the {kind} message: {error.read()[:200]!r}')
                return message
            except (urllib.error.URLError, OSError) as error:
                print(f'::warning::Discord could not be reached for the {kind} message: {error}')
                return message
        return message

    def changes(self, previous, sha):
        """the first changes as Discord lines"""
        items = nightly_notes.changes(self.checkout, previous, sha) or []
        lines = [f'- {item}' for item in items[:10]] + ([f'- and {len(items) - 10} more'] if len(items) > 10 else [])
        return '\n'.join(lines) or f'Built from {sha[:7]}.'

    def older(self, sha, previous):
        """sha is an ancestor of previous, a newer commit already published"""
        if not previous or previous == sha:
            return False
        command = ['git', '-C', self.checkout, 'merge-base', '--is-ancestor', sha, previous]
        return subprocess.run(command, capture_output=True, check=False).returncode == 0

    # ---- what each event publishes
    def nightly(self, run, sha):
        files, uploads = self.complete(run, sha)
        date = nightly_notes.git(self.checkout, 'log', '-1', '--format=%cs', sha).strip()
        tag = f'nightly-{date}-{sha[:7]}'
        older = sorted(
            (r for r in self.releases(self.to) if r['tag_name'].startswith('nightly-') and r['tag_name'] != tag),
            key=lambda r: r['created_at'],
            reverse=True,
        )
        previous = next((m.group(0) for r in older[:1] for m in [re.search(r'[0-9a-f]{40}', r['body'] or '')] if m), '')
        if self.older(sha, previous):  # a run that ended after a newer commit's: that one stays the latest
            print(f'{sha[:7]} is older than the newest nightly, {previous[:7]}: not published')
            return
        kept = (
            f'This repository keeps the newest {self.keep} nightlies; [the latest]({SERVER}/{self.to}/releases/'
            'latest) is always the newest one.'
        )
        download = f'{SERVER}/{self.to}/releases/download/{tag}'
        body = nightly_notes.notes(self.checkout, sha, previous, self.repo, list(files), download, kept)
        title = nightly_notes.title(self.checkout, sha)
        made, new = self.put_release(self.to, tag, title, body, False, uploads)
        for old in older[self.keep - 1 :]:
            self.drop_release(self.to, old)
            print(f'deleted {old["tag_name"]}: past the newest {self.keep}')
        print(f'{"published" if new else "replaced the files of"} {tag}')
        url = made.get('html_url', f'{SERVER}/{self.to}/releases/tag/{tag}')
        self.point_rolling(title, url, sha)
        if new:  # a run again of the same commit is no news
            self.discord(
                'nightly', {'title': title, 'url': url, 'description': self.changes(previous, sha), 'color': SCARLET}
            )

    def point_rolling(self, title, url, sha):
        """REPO's rolling prerelease ROLLING: no files, the link to the latest nightly, its tag on that commit"""
        rolling = self.release(self.repo, ROLLING)
        fields = {
            'name': f'{title}, in {self.to}',
            'body': (
                f'The nightlies are in [{self.to}]({SERVER}/{self.to}/releases), the newest {self.keep} kept. The '
                f'latest is [{title}]({url}), and {SERVER}/{self.to}/releases/latest always leads to the newest one.\n'
            ),
            'prerelease': True,
            'make_latest': 'false',
        }
        if not rolling:
            self.write(
                self.repo, 'POST', f'repos/{self.repo}/releases', dict(fields, tag_name=ROLLING, target_commitish=sha)
            )
        else:
            self.write(self.repo, 'PATCH', f'repos/{self.repo}/git/refs/tags/{ROLLING}', {'sha': sha, 'force': True})
            self.write(self.repo, 'PATCH', f'repos/{self.repo}/releases/{rolling["id"]}', fields)
            for asset in rolling.get('assets', []):
                self.write(self.repo, 'DELETE', f'repos/{self.repo}/releases/assets/{asset["id"]}')
        print(f'{self.repo} "{ROLLING}" links {title}')

    def version(self, run, sha, tag):
        files, uploads = self.complete(run, sha)
        earlier = sorted(
            (
                r
                for r in self.releases(self.repo)
                if VERSION.fullmatch(r['tag_name']) and r['tag_name'] != tag and not r.get('draft')
            ),
            key=lambda r: r['created_at'],
            reverse=True,
        )
        since = earlier[0]['tag_name'] if earlier else ''
        previous = nightly_notes.git(self.checkout, 'rev-list', '-n', '1', since).strip() if since else ''
        download = f'{SERVER}/{self.repo}/releases/download/{tag}'
        body = nightly_notes.notes(self.checkout, sha, previous, self.repo, list(files), download, '', since)
        title = f'ScarletNX {tag}'
        made, new = self.put_release(self.repo, tag, title, body, '-' in tag, uploads, sha)
        print(f'{"published" if new else "replaced the files of"} {self.repo} {tag}')
        if new:
            url = made.get('html_url', f'{SERVER}/{self.repo}/releases/tag/{tag}')
            self.discord(
                'releases', {'title': title, 'url': url, 'description': self.changes(previous, sha), 'color': SCARLET}
            )

    def pull_notes(self, pr, sha, files, missing, message):
        url = f'{SERVER}/{self.repo}'
        commit = f'{url}/pull/{pr["number"]}/commits/{sha}'
        lines = [
            (
                f'Builds of [#{pr["number"]}]({pr["html_url"]}), {pr["title"]}, by @{pr["user"]["login"]}, at '
                f"[`{sha[:7]}`]({commit}). A pull request's builds, not a release: each push to it replaces them, "
                'and they go when it closes.'
            ),
            '',
        ]
        if files:
            lines += nightly_notes.table(list(files), f'{SERVER}/{self.to}/releases/download/pr-{pr["number"]}')
        if missing:
            lines += ['', f'Not built: {"; ".join(missing)}.']
        lines += ['', *nightly_notes.source(commit)]
        if message:
            lines += ['', f'<!-- discord-message: {message} -->']
        return '\n'.join(lines) + '\n'

    def pull(self, pr, sha):
        number = pr['number']
        if pr['head']['sha'] != sha:
            print(f'#{number} is at {pr["head"]["sha"][:7]} now: the builds of {sha[:7]} are not published')
            return
        files, uploads, missing = self.packages(sha)
        if not files:
            print(f'#{number}: nothing of {sha[:7]} was built, so its builds stay as they were')
            return
        tag = f'pr-{number}'
        existing = self.release(self.to, tag)
        message = next(iter(DISCORD.findall((existing or {}).get('body') or '')), None)
        title = f'PR #{number}: {pr["title"]}'
        body = self.pull_notes(pr, sha, files, missing, message)
        made, new = self.put_release(self.to, tag, title, body, True, uploads)
        print(f'{"published" if new else "replaced"} {tag} with the builds of {sha[:7]}')
        said = f'by {pr["user"]["login"]}, at [`{sha[:7]}`]({SERVER}/{self.repo}/pull/{number}/commits/{sha})'
        embed = {
            'title': title[:256],
            'url': made.get('html_url', f'{SERVER}/{self.to}/releases/tag/{tag}'),
            'description': said + (f'\nNot built: {"; ".join(missing)}' if missing else ''),
            'color': SCARLET,
        }
        posted = self.discord('prs', embed, message)
        if posted and posted != message:  # kept for the next push to edit it
            body = self.pull_notes(pr, sha, files, missing, posted)
            self.write(self.to, 'PATCH', f'repos/{self.to}/releases/{made.get("id")}', {'body': body})

    def closed(self, pr):
        tag = f'pr-{pr["number"]}'
        release = self.release(self.to, tag)
        if not release:
            print(f'#{pr["number"]} closed: it had no builds in {self.to}')
            return
        message = next(iter(DISCORD.findall(release.get('body') or '')), None)
        self.drop_release(self.to, release)
        print(f'deleted {tag}: #{pr["number"]} closed')
        if message:
            embed = {
                'title': f'PR #{pr["number"]}: {pr["title"]}'[:256],
                'url': pr['html_url'],
                'description': f'{"Merged" if pr.get("merged") else "Closed"}: its builds are gone.',
            }
            self.discord('prs', embed, message)

    # ---- who may publish a fork's pull request
    def fork(self, pr):
        return pr['head']['repo'] is None or pr['head']['repo']['full_name'] != self.repo

    def approved(self, pr, since):
        """labeled LABEL by the team after since (when the run of its head began), and still labeled"""
        if LABEL not in {label['name'] for label in pr.get('labels', [])}:
            return False
        events = self.pages(f'repos/{self.repo}/issues/{pr["number"]}/events', self.repo)
        for event in reversed(events):
            if event.get('event') != 'labeled' or event.get('label', {}).get('name') != LABEL:
                continue
            if event['created_at'] < since:
                return False
            login = event['actor']['login']
            try:
                permission = self.read(f'repos/{self.repo}/collaborators/{login}/permission').get('permission')
            except RuntimeError as error:
                permission = f'unknown: {error}'
            if permission in TEAM:
                return True
            print(f'"{LABEL}" was put by {login}, who is not of the team ({permission})')
        return False

    def pull_of(self, sha):
        """the open pull request whose head is sha"""
        pulls = self.pages(f'repos/{self.repo}/pulls?state=open', self.repo)
        return next((pr for pr in pulls if pr['head']['sha'] == sha), None)

    def take(self, kind, run, pr=None, tag=''):
        """the run publish takes the artifacts of, for the workflow to download them"""
        self.output = {
            'kind': kind,
            'run': str(run['id']),
            'sha': run['head_sha'],
            'short': run['head_sha'][:7],
            'pr': str(pr['number']) if pr else '',
            'tag': tag,
        }
        print(f'to publish: the {kind} builds of run {run["id"]} ({run["head_sha"][:7]})')

    def tagged(self, name):
        """name is a tag of REPO (a run's head_branch is the tag a tag push pushed)"""
        try:
            return bool(self.read(f'repos/{self.repo}/git/ref/tags/{urllib.parse.quote(name)}'))
        except RuntimeError:
            return False

    def on_run(self, run):
        if run['event'] == 'push':
            name = run['head_branch']
            if run['head_repository']['full_name'] != self.repo:
                print(f'a push to {run["head_repository"]["full_name"]}: nothing to publish')
            elif name != 'main' and not (VERSION.fullmatch(name) and self.tagged(name)):
                print(f'a push to {name}: neither main nor a version tag, nothing to publish')
            elif run['conclusion'] != 'success':
                print(f'run {run["id"]} of {name} ended {run["conclusion"]}: nothing published')
            elif name == 'main':
                self.take('nightly', run)
            else:
                self.take('release', run, tag=name)
            return
        if run['event'] != 'pull_request':
            print(f'a run of {run["event"]}: nothing to publish')
            return
        if run['conclusion'] == 'cancelled':
            print(f'run {run["id"]} was cancelled: a newer push has its own')
            return
        pr = self.pull_of(run['head_sha'])
        if not pr:
            print(f"{run['head_sha'][:7]} is no open pull request's head: nothing to publish")
        elif self.fork(pr) and not self.approved(pr, run['created_at']):
            print(f'#{pr["number"]} is a fork\'s: it waits for the team\'s "{LABEL}" label on this push')
        else:
            self.take('pr', run, pr)

    def on_pull(self, action, pr, label):
        if action == 'closed':
            self.closed(pr)
        elif action == 'synchronize' and self.fork(pr) and LABEL in {lb['name'] for lb in pr.get('labels', [])}:
            # what was let through was the push before: this one waits for the label again
            path = f'repos/{self.repo}/issues/{pr["number"]}/labels/{urllib.parse.quote(LABEL)}'
            self.gh(['api', '-X', 'DELETE', path], self.repo, write=True)
            print(f'took "{LABEL}" off #{pr["number"]}: a new push')
        elif action == 'labeled' and label == LABEL and self.fork(pr):
            path = f'repos/{self.repo}/actions/workflows/build.yml/runs?head_sha={pr["head"]["sha"]}'
            done = sorted(
                (r for r in self.pages(path, self.repo) if r['status'] == 'completed' and r['event'] == 'pull_request'),
                key=lambda r: r['created_at'],
            )
            if not done:
                print(f'#{pr["number"]}: its build has not ended; it is published when it does')
            elif self.approved(pr, done[-1]['created_at']):
                self.take('pr', done[-1], pr)
        else:
            print(f'pull request {action}: nothing to publish')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('step', choices=['plan', 'publish'])
    parser.add_argument('--repo', required=True)
    parser.add_argument('--to', required=True, help='the nightly repository')
    parser.add_argument('--kind', choices=['nightly', 'pr', 'release'])
    parser.add_argument('--run', help='the Build run the artifacts are of')
    parser.add_argument('--sha', help='the commit it built')
    parser.add_argument('--pr', type=int, help="the pull request's number")
    parser.add_argument('--tag', help="the release's version tag")
    parser.add_argument('--artifacts', help='the folder the workflow downloaded the artifacts into')
    parser.add_argument('--checkout', default='.', help="REPO's history and tags, for the notes")
    parser.add_argument('--keep', type=int, default=30, help='the nightlies kept')
    parser.add_argument('--dry-run', action='store_true', help='read, and print each write instead')
    args = parser.parse_args()
    if args.keep < 1:
        parser.error('--keep takes 1 or more')
    if args.step == 'publish' and not (args.kind and args.run and args.sha and args.artifacts):
        parser.error('publish takes --kind, --run, --sha and --artifacts')
    if args.kind == 'pr' and not args.pr:
        parser.error('--kind pr takes --pr')
    if args.kind == 'release' and not (args.tag and VERSION.fullmatch(args.tag)):
        parser.error('--kind release takes a version --tag (v...)')
    publisher = Publisher(args)
    try:
        if args.step == 'publish':
            if args.kind == 'nightly':
                publisher.nightly(args.run, args.sha)
            elif args.kind == 'release':
                publisher.version(args.run, args.sha, args.tag)
            else:
                publisher.pull(publisher.read(f'repos/{args.repo}/pulls/{args.pr}'), args.sha)
            return
        name = os.environ['GITHUB_EVENT_NAME']
        with open(os.environ['GITHUB_EVENT_PATH'], encoding='utf-8') as file:
            event = json.load(file)
        if name == 'workflow_run':
            publisher.on_run(event['workflow_run'])
        elif name == 'pull_request_target':
            publisher.on_pull(event['action'], event['pull_request'], event.get('label', {}).get('name'))
        else:
            print(f'{name}: nothing to publish')
        if publisher.output and os.environ.get('GITHUB_OUTPUT'):
            with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as file:
                file.writelines(f'{key}={value}\n' for key, value in publisher.output.items())
    finally:
        shutil.rmtree(publisher.work, ignore_errors=True)


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as error:
        sys.exit(f'publish-builds: {error}')
