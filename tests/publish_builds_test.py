#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/publish-builds.py against a fake gh (a small GitHub kept in a JSON file) and a fake Discord (a local HTTP
server): a version tag's release in the repository; the nightly made in the nightly repository, kept to the newest N,
never one older than the latest, and linked from the repository's rolling prerelease; a pull request's prerelease
replaced at each push and gone when it closes, its one Discord message edited; a fork's only once the team labels it
after its last push; and the writes to the nightly repository made with its own token. Offline."""

import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / 'scripts' / 'publish-builds.py'
REPO, TO = 'owner/repo', 'owner/nightly'
FILES = [
    'ScarletNX-linux-x86_64.AppImage',
    'ScarletNX-linux-aarch64.AppImage',
    'ScarletNX-windows-x86_64.zip',
    'ScarletNX-android-universal.apk',
]
ZSYNC = ['ScarletNX-linux-x86_64.AppImage.zsync', 'ScarletNX-linux-aarch64.AppImage.zsync']
failures = []


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what}{": " + str(detail) if detail else ""}')


# releases and tags of each repository, under its name
FAKE_GH = r"""#!/usr/bin/env python3
import json, os, re, sys
path = os.environ['FAKE_GH_STATE']
with open(path) as f:
    s = json.load(f)
args = sys.argv[1:]
with open(os.environ['FAKE_GH_LOG'], 'a') as log:
    log.write(json.dumps({'args': args, 'token': os.environ.get('GH_TOKEN')}) + '\n')
def save():
    with open(path, 'w') as f:
        json.dump(s, f)
def out(value):
    print(json.dumps(value))
def stop(text):
    sys.stderr.write(text + '\n')
    sys.exit(1)
if args[:2] == ['release', 'upload']:
    end = args.index('--repo')
    repo = args[end + 1]
    release = next((r for r in s['releases'][repo] if r['tag_name'] == args[2]), None) or stop('release not found')
    for file in args[3:end]:
        name = os.path.basename(file)
        release['assets'] = [a for a in release['assets'] if a['name'] != name]
        s['next'] += 1
        with open(file, 'rb') as f:
            release['assets'].append({'id': s['next'], 'name': name, 'data': f.read()[:64].decode('latin-1')})
    save()
    sys.exit(0)
assert args[0] == 'api', args
method = args[args.index('-X') + 1] if '-X' in args else 'GET'
target = [a for a in args[1:] if not a.startswith('-') and a not in ('GET', 'POST', 'PATCH', 'DELETE')][0]
route, _, query = target.partition('?')
query = dict(p.split('=', 1) for p in query.split('&') if p)
data = json.load(sys.stdin) if '--input' in args else None
page, per = int(query.get('page', 1)), int(query.get('per_page', 30))
def listing(items):
    out(items[(page - 1) * per:page * per])
m = re.fullmatch(r'repos/([^/]+/[^/]+)/(.*)', route)
repo, rest = m.group(1), m.group(2)
releases, tags = s['releases'].setdefault(repo, []), s['tags'].setdefault(repo, {})
if rest == 'releases' and method == 'GET':
    listing(sorted(releases, key=lambda r: r['created_at'], reverse=True))
elif rest == 'releases' and method == 'POST':
    s['next'] += 1
    s['clock'] += 1
    release = dict(data, id=s['next'], created_at=f'2026-11-01T{s["clock"]:08d}', assets=[],
                   html_url=f'https://github.com/{repo}/releases/tag/{data["tag_name"]}')
    releases.append(release)
    tags.setdefault(data['tag_name'], data.get('target_commitish', 'default-branch'))
    save()
    out(release)
elif rest.startswith('releases/assets/') and method == 'DELETE':
    number = int(rest.split('/')[-1])
    for release in releases:
        release['assets'] = [a for a in release['assets'] if a['id'] != number]
    save()
elif rest.startswith('releases/') and method in ('PATCH', 'DELETE'):
    number = int(rest.split('/')[-1])
    release = next((r for r in releases if r['id'] == number), None) or stop('no such release')
    if method == 'DELETE':
        releases.remove(release)
    else:
        release.update(data)
        out(release)
    save()
elif rest.startswith('git/refs/tags/') and method in ('DELETE', 'PATCH'):
    tag = rest[len('git/refs/tags/'):]
    if tag not in tags:
        stop('gh: Reference does not exist (HTTP 422)')
    if method == 'DELETE':
        del tags[tag]
    else:
        tags[tag] = data['sha']
    save()
elif rest.startswith('git/ref/tags/'):
    tag = rest[len('git/ref/tags/'):]
    out({'ref': f'refs/tags/{tag}', 'object': {'sha': tags[tag]}}) if tag in tags else stop('gh: Not Found (HTTP 404)')
elif rest == 'pulls' and method == 'GET':
    listing([p for p in s['pulls'] if p['state'] == 'open'])
elif re.fullmatch(r'pulls/\d+', rest):
    out(next(p for p in s['pulls'] if p['number'] == int(rest.split('/')[1])))
elif re.fullmatch(r'issues/\d+/events', rest):
    listing(s['events'].get(rest.split('/')[1], []))
elif re.fullmatch(r'issues/\d+/labels/.+', rest) and method == 'DELETE':
    number, label = int(rest.split('/')[1]), rest.split('/', 3)[3].replace('%20', ' ')
    pull = next(p for p in s['pulls'] if p['number'] == number)
    pull['labels'] = [l for l in pull['labels'] if l['name'] != label]
    save()
elif re.fullmatch(r'collaborators/[^/]+/permission', rest):
    out({'permission': s['permissions'].get(rest.split('/')[1], 'read')})
elif rest == 'actions/workflows/build.yml/runs':
    out({'workflow_runs': [r for r in s['runs'] if r['head_sha'] == query.get('head_sha')]})
else:
    stop(f'fake gh: {method} {target}')
"""


class Discord(http.server.BaseHTTPRequestHandler):
    requests, messages, next_id = [], {}, [1000]

    def log_message(self, *args):
        pass

    def reply(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_one(self, method):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        Discord.requests.append(
            {'method': method, 'path': self.path, 'body': body, 'agent': self.headers['User-Agent']}
        )
        path = self.path.split('?')[0]
        if method == 'POST':
            Discord.next_id[0] += 1
            Discord.messages[str(Discord.next_id[0])] = body
            self.reply(200, {'id': str(Discord.next_id[0])})
        elif path.split('/')[-1] in Discord.messages:
            Discord.messages[path.split('/')[-1]] = body
            self.reply(200, {'id': path.split('/')[-1]})
        else:
            self.reply(404, {'message': 'Unknown Message'})

    def do_POST(self):
        self.handle_one('POST')

    def do_PATCH(self):
        self.handle_one('PATCH')


def posted(channel):
    return [r for r in Discord.requests if f'token-{channel}' in r['path']]


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Discord)
threading.Thread(target=server.serve_forever, daemon=True).start()
HOOK = f'http://127.0.0.1:{server.server_address[1]}/api/webhooks/1/token'

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    bin_dir, checkout = root / 'bin', root / 'checkout'
    bin_dir.mkdir()
    (bin_dir / 'gh').write_text(FAKE_GH)
    (bin_dir / 'gh').chmod(0o755)
    state_file, log_file = root / 'state.json', root / 'gh.log'

    def git(*args):
        env = dict(os.environ, GIT_COMMITTER_DATE='2026-10-15T12:00:00Z', GIT_AUTHOR_DATE='2026-10-15T12:00:00Z')
        return subprocess.run(
            ['git', '-C', str(checkout), '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false']
            + ['-c', 'core.hooksPath=' + os.devnull, '-c', 'tag.gpgsign=false', *args],
            check=True,
            capture_output=True,
            text=True,
            env=env,
        ).stdout.strip()

    checkout.mkdir()
    git('init', '-q', '-b', 'main')
    git('commit', '-q', '--allow-empty', '-m', 'the older nightly')
    oldest = git('rev-parse', 'HEAD')
    git('commit', '-q', '--allow-empty', '-m', 'the previous nightly')
    previous = git('rev-parse', 'HEAD')
    git('tag', 'v0.1.0')
    git('switch', '-q', '-c', 'feature')
    git('commit', '-q', '--allow-empty', '-m', '[cpu] a commit of the pull request')
    git('switch', '-q', 'main')
    git('merge', '-q', '--no-ff', 'feature', '-m', 'Merge pull request #5 from someone/feature\n\n[ci] its title')
    sha = git('rev-parse', 'HEAD')
    git('tag', 'v0.2.0')

    def save_state(state):
        state_file.write_text(json.dumps(state))

    def state():
        return json.loads(state_file.read_text())

    def releases(repo=TO, s=None):
        return (s or state())['releases'].get(repo, [])

    def find(tag, repo=TO, s=None):
        return next((r for r in releases(repo, s) if r['tag_name'] == tag), None)

    def with_releases(repo, items):
        return dict(state(), releases=dict(state()['releases'], **{repo: items}))

    def calls():
        return [json.loads(line) for line in log_file.read_text().splitlines()] if log_file.exists() else []

    def artifacts(name, commit, skip=()):
        folder = root / name
        short = commit[:7]
        made = {
            f'ScarletNX-linux-x86_64-{short}.AppImage': {
                'ScarletNX-linux-x86_64.AppImage': 'elf x86',
                'ScarletNX-linux-x86_64.AppImage.zsync': 'zsync x86',
                'appinfo': 'i',
            },
            f'ScarletNX-linux-aarch64-{short}.AppImage': {
                'ScarletNX-linux-aarch64.AppImage': 'elf arm',
                'ScarletNX-linux-aarch64.AppImage.zsync': 'zsync arm',
            },
            f'ScarletNX-windows-x86_64-{short}': {'bin/scarletnx.exe': 'MZ', 'share/licenses/LICENSE': 'GPL'},
            f'ScarletNX-android-universal-{short}.apk': {f'ScarletNX-android-universal-{short}.apk': 'PK apk'},
        }
        for artifact, files in made.items():
            for file, text in files.items():
                if any(part in f'{artifact}/{file}' for part in skip):
                    continue
                (folder / artifact / file).parent.mkdir(parents=True, exist_ok=True)
                (folder / artifact / file).write_text(text)
        return folder

    def run(step, *extra, event=None, name='workflow_run', hooks=True, env_extra=None):
        event_file, output = root / 'event.json', root / 'output'
        event_file.write_text(json.dumps(event or {}))
        output.write_text('')
        env = dict(
            os.environ,
            PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}',
            FAKE_GH_STATE=str(state_file),
            FAKE_GH_LOG=str(log_file),
            GH_TOKEN='repo-token',
            NIGHTLY_TOKEN='nightly-token',
            GITHUB_EVENT_NAME=name,
            GITHUB_EVENT_PATH=str(event_file),
            GITHUB_OUTPUT=str(output),
        )
        for kind in ('RELEASES', 'NIGHTLY', 'PRS'):
            env.pop(f'DISCORD_WEBHOOK_{kind}', None)
        if hooks:
            env.update(
                DISCORD_WEBHOOK_RELEASES=f'{HOOK}-releases',
                DISCORD_WEBHOOK_NIGHTLY=f'{HOOK}-nightly',
                DISCORD_WEBHOOK_PRS=f'{HOOK}-prs?thread_id=55',
            )
        env.update(env_extra or {})
        done = subprocess.run(
            [sys.executable, str(SCRIPT), step, '--repo', REPO, '--to', TO, *extra],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        planned = dict(line.split('=', 1) for line in output.read_text().splitlines())
        return done, planned

    def push_run(commit, conclusion='success', branch='main', number=1):
        return {
            'workflow_run': {
                'id': number,
                'event': 'push',
                'head_branch': branch,
                'head_sha': commit,
                'conclusion': conclusion,
                'created_at': '2026-10-16T10:00:00Z',
                'head_repository': {'full_name': REPO},
            }
        }

    def nightly_release(commit, created, number):
        return {
            'id': number,
            'tag_name': f'nightly-2026-10-15-{commit[:7]}',
            'name': 'n',
            'created_at': created,
            'body': f'Built from [`{commit[:7]}`](https://github.com/{REPO}/commit/{commit}).',
            'assets': [],
            'prerelease': False,
        }

    def everything(commit, run_id='41'):
        return ['--run', run_id, '--sha', commit, '--artifacts', str(artifacts('full', commit))]

    def publish(kind, *extra, **options):
        return run('publish', '--kind', kind, *extra, **options)[0]

    # ---- nightlies: in the nightly repository, linked from the rolling prerelease
    rolling = {
        'id': 9,
        'tag_name': 'nightly',
        'name': 'Nightly',
        'created_at': '2026-09-01',
        'body': 'old',
        'prerelease': True,
        'assets': [{'id': 7, 'name': 'ScarletNX-x86_64.AppImage', 'data': ''}],
    }
    save_state(
        {
            'next': 100,
            'clock': 0,
            'pulls': [],
            'events': {},
            'permissions': {},
            'runs': [],
            'tags': {
                TO: {f'nightly-2026-10-15-{c[:7]}': 'x' for c in (oldest, previous)},
                REPO: {'nightly': oldest, 'v0.1.0': previous, 'v0.2.0': sha},
            },
            'releases': {
                TO: [nightly_release(oldest, '2026-10-01', 1), nightly_release(previous, '2026-10-02', 2)],
                REPO: [rolling],
            },
        }
    )
    done, planned = run('plan', event=push_run(sha, number=41))
    check(
        'a green push to main is planned as a nightly',
        planned == {'kind': 'nightly', 'run': '41', 'sha': sha, 'short': sha[:7], 'pr': '', 'tag': ''},
        planned,
    )
    for event, why in (
        (push_run(sha, 'failure'), 'a failed run'),
        (push_run(sha, branch='dev'), 'another branch'),
        (push_run(sha, branch='v9.9.9'), 'a branch named like a version, no tag'),
    ):
        done, planned = run('plan', event=event)
        check(f'{why} is not planned', not planned and done.returncode == 0, done.stdout + done.stderr)
    log_file.unlink(missing_ok=True)
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '2')
    check('the nightly publishes', done.returncode == 0, done.stdout + done.stderr)
    s = state()
    tag = f'nightly-2026-10-15-{sha[:7]}'
    made = find(tag, s=s)
    check(
        'the release made, the latest, not a prerelease',
        made and made['make_latest'] == 'true' and not made['prerelease'],
        made,
    )
    check(
        "its four files, and the AppImages' .zsync",
        made and sorted(a['name'] for a in made['assets']) == sorted(FILES + ZSYNC),
        made and made['assets'],
    )
    check(
        'its notes: the files linked from this release, the changes, the source',
        made
        and f'https://github.com/{TO}/releases/download/{tag}/ScarletNX-android-universal.apk' in made['body']
        and '- [ci] its title (#5)' in made['body']
        and f'https://github.com/{REPO}/tree/{sha}' in made['body']
        and 'newest 2 nightlies' in made['body']
        and 'zsync' not in made['body'],
        made and made['body'],
    )
    archive = next((a['data'] for a in made['assets'] if a['name'].endswith('.zip')), '') if made else ''
    check('the Windows build zipped from its folder', archive.startswith('PK') and 'bin/scarletnx.exe' in archive)
    check(
        'only the newest 2 kept, the oldest and its tag gone',
        sorted(r['tag_name'] for r in releases(s=s)) == sorted([tag, f'nightly-2026-10-15-{previous[:7]}'])
        and f'nightly-2026-10-15-{oldest[:7]}' not in s['tags'][TO],
        s['tags'][TO],
    )
    linked = find('nightly', REPO, s)
    check(
        'the rolling prerelease: no files, a link to the latest, its tag moved there',
        linked
        and not linked['assets']
        and linked['prerelease']
        and f'https://github.com/{TO}/releases/tag/{tag}' in linked['body']
        and s['tags'][REPO]['nightly'] == sha,
        linked,
    )
    writes = [c for c in calls() if '-X' in c['args'] or c['args'][0] == 'release']
    to_writes = [c for c in writes if TO in ' '.join(c['args'])]
    repo_writes = [c for c in writes if REPO in ' '.join(c['args'])]
    check(
        'the nightly repository written with its token, the repository with its own',
        to_writes
        and repo_writes
        and all(c['token'] == 'nightly-token' for c in to_writes)
        and all(c['token'] == 'repo-token' for c in repo_writes),
        writes,
    )
    sent = posted('nightly')
    check(
        'one Discord message on the nightly channel, its title and changes',
        len(sent) == 1
        and sent[0]['method'] == 'POST'
        and sent[0]['path'].endswith('?wait=true')
        and sent[0]['body']['embeds'][0]['title'] == f'Nightly 2026-10-15 ({sha[:7]})'
        and '[ci] its title (#5)' in sent[0]['body']['embeds'][0]['description']
        and sent[0]['agent'].startswith('DiscordBot'),
        sent,
    )
    # the run again: the files replaced, no news
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--keep', '2')
    check(
        'a run again replaces, posts nothing more',
        done.returncode == 0 and len(posted('nightly')) == 1 and len(releases()) == 2,
        done.stdout + done.stderr,
    )
    # a run of an older commit ending last: not published over the newest
    older_files = ['--run', '40', '--sha', oldest, '--artifacts', str(artifacts('old', oldest))]
    done = publish('nightly', *older_files, '--checkout', str(checkout))
    check(
        'an older commit is not published',
        done.returncode == 0
        and 'older than the newest nightly' in done.stdout
        and all(oldest[:7] not in r['tag_name'] for r in releases()),
        done.stdout + done.stderr,
    )
    # a nightly without one of its packages, or an AppImage without its .zsync, stops, saying which
    for skip, said in (('ScarletNX-windows', 'Windows, x86-64'), ('AppImage.zsync', 'has no ScarletNX-linux')):
        partial = ['--run', '42', '--sha', sha, '--artifacts', str(artifacts(f'part-{len(skip)}', sha, (skip,)))]
        done = publish('nightly', *partial, '--checkout', str(checkout))
        check(f'a nightly missing {skip} stops', done.returncode != 0 and said in done.stderr, done.stderr)
    # dry run: reads only
    log_file.unlink(missing_ok=True)
    save_state(with_releases(TO, [r for r in releases() if r['tag_name'] != tag]))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout), '--dry-run')
    check(
        '--dry-run writes nothing',
        done.returncode == 0
        and not any('-X' in c['args'] or c['args'][0] == 'release' for c in calls())
        and 'would run: gh api -X POST' in done.stdout,
        done.stdout + done.stderr,
    )
    # no rolling prerelease yet: made, on the commit
    save_state(with_releases(REPO, []))
    done = publish('nightly', *everything(sha), '--checkout', str(checkout))
    linked = find('nightly', REPO)
    check(
        'a rolling prerelease made when there is none',
        done.returncode == 0 and linked and linked['target_commitish'] == sha and linked['prerelease'],
        done.stdout + done.stderr,
    )

    # ---- a version tag: the repository's release
    first = {
        'id': 50,
        'tag_name': 'v0.1.0',
        'name': 'ScarletNX v0.1.0',
        'created_at': '2026-10-01',
        'body': '',
        'prerelease': False,
        'assets': [],
    }
    save_state(with_releases(REPO, [*releases(REPO), first]))
    done, planned = run('plan', event=push_run(sha, branch='v0.2.0', number=43))
    check(
        'a version tag pushed is planned as a release',
        planned.get('kind') == 'release' and planned.get('tag') == 'v0.2.0',
        done.stdout + done.stderr,
    )
    done = publish('release', '--tag', 'v0.2.0', *everything(sha), '--checkout', str(checkout))
    version = find('v0.2.0', REPO)
    check(
        "the repository's release: the latest, on the commit, its files and their .zsync",
        done.returncode == 0
        and version
        and version['make_latest'] == 'true'
        and not version['prerelease']
        and version['target_commitish'] == sha
        and sorted(a['name'] for a in version['assets']) == sorted(FILES + ZSYNC),
        done.stdout + done.stderr,
    )
    check(
        'its notes: the changes since the previous version, its files',
        version
        and '### Changes since v0.1.0' in version['body']
        and '- [ci] its title (#5)' in version['body']
        and f'https://github.com/{REPO}/releases/download/v0.2.0/ScarletNX-windows-x86_64.zip' in version['body'],
        version and version['body'],
    )
    sent = posted('releases')
    check(
        'one message on the releases channel',
        len(sent) == 1 and sent[0]['body']['embeds'][0]['title'] == 'ScarletNX v0.2.0',
        sent,
    )
    git('tag', 'v0.3.0-rc1')
    done = publish('release', '--tag', 'v0.3.0-rc1', *everything(sha), '--checkout', str(checkout))
    candidate = find('v0.3.0-rc1', REPO)
    check(
        'a tag with a "-" is a prerelease, never the latest',
        candidate and candidate['prerelease'] and candidate['make_latest'] == 'false',
        done.stdout + done.stderr,
    )
    done = publish('release', '--tag', 'main', *everything(sha))
    check('--kind release refuses a tag that is no version', done.returncode == 2, done.stderr)

    # ---- a pull request of the repository
    def pull(number, head, fork=False, labels=()):
        return {
            'number': number,
            'state': 'open',
            'title': 'Fix it @everyone',
            'html_url': f'https://x/pull/{number}',
            'user': {'login': 'alice'},
            'labels': [{'name': n} for n in labels],
            'merged': False,
            'head': {'sha': head, 'repo': {'full_name': 'alice/fork' if fork else REPO}},
        }

    def pr_run(head, number=50, created='2026-10-16T10:00:00Z'):
        return {
            'id': number,
            'event': 'pull_request',
            'head_branch': 'x',
            'head_sha': head,
            'conclusion': 'failure',
            'status': 'completed',
            'created_at': created,
            'head_repository': {'full_name': REPO},
        }

    def pr_files(head, name, run_id, skip=()):
        return ['--run', run_id, '--sha', head, '--pr', '7', '--artifacts', str(artifacts(name, head, skip))]

    head1, head2 = 'a' * 40, 'b' * 40
    save_state(dict(state(), pulls=[pull(7, head1)]))
    done, planned = run('plan', event={'workflow_run': pr_run(head1)})
    check('a pull request run is planned', planned.get('kind') == 'pr' and planned.get('pr') == '7', planned)
    done = publish('pr', *pr_files(head1, 'pr1-part', '50', ('ScarletNX-windows',)))
    pre = find('pr-7')
    check(
        'its prerelease, never the latest, titled PR #N',
        pre and pre['prerelease'] and pre['make_latest'] == 'false' and pre['name'] == 'PR #7: Fix it @everyone',
        pre,
    )
    check(
        'what was not built said',
        pre and 'Not built: Windows, x86-64.' in pre['body'] and len(pre['assets']) == 5,
        pre,
    )
    prs = posted('prs')
    check(
        'one message on the pull requests channel, in its thread, no one pinged',
        len(prs) == 1
        and prs[0]['path'].endswith('?thread_id=55&wait=true')
        and prs[0]['body']['allowed_mentions'] == {'parse': []}
        and prs[0]['body']['embeds'][0]['title'] == 'PR #7: Fix it @everyone',
        prs,
    )
    message = str(Discord.next_id[0])
    check("the message's id kept in the notes", pre and f'<!-- discord-message: {message} -->' in pre['body'], pre)
    # a push: the same prerelease, all four now, the message edited
    save_state(dict(state(), pulls=[pull(7, head2)]))
    done = publish('pr', *pr_files(head2, 'pr2', '51'))
    pre = find('pr-7')
    prs = posted('prs')
    check(
        'the push replaces it',
        pre and len(pre['assets']) == 6 and head2[:7] in pre['body'] and 'Not built' not in pre['body'],
        pre,
    )
    check(
        'and edits the one message',
        len(prs) == 2 and prs[1]['method'] == 'PATCH' and prs[1]['path'].endswith(f'/messages/{message}?thread_id=55'),
        prs,
    )
    # the run of the push before, ending last
    done = publish('pr', *pr_files(head1, 'pr1', '50'))
    check(
        "an older push's builds are not published",
        'are not published' in done.stdout and head2[:7] in find('pr-7')['body'],
        done.stdout,
    )
    # a message deleted on Discord: a new one, its id kept
    Discord.messages.pop(message)
    done = publish('pr', *pr_files(head2, 'pr2', '51'))
    pre = find('pr-7')
    check(
        'a deleted message is posted again',
        f'<!-- discord-message: {Discord.next_id[0]} -->' in pre['body'] and str(Discord.next_id[0]) != message,
        pre['body'],
    )
    # closed, merged: gone, the message says so
    closed = dict(pull(7, head2), state='closed', merged=True)
    done, planned = run('plan', name='pull_request_target', event={'action': 'closed', 'pull_request': closed})
    s = state()
    check(
        'closed: its prerelease and tag gone',
        'pr-7' not in s['tags'][TO] and not find('pr-7', s=s) and not planned,
        done.stdout + done.stderr,
    )
    last = posted('prs')[-1]
    check(
        'its message edited: merged',
        last['method'] == 'PATCH' and last['body']['embeds'][0]['description'].startswith('Merged'),
        last,
    )

    # ---- a fork's pull request
    def labeled(login, at):
        return {'event': 'labeled', 'label': {'name': 'publish builds'}, 'actor': {'login': login}, 'created_at': at}

    fork_head = 'c' * 40
    base = dict(
        state(),
        pulls=[pull(9, fork_head, fork=True)],
        permissions={'bob': 'write', 'eve': 'read'},
        runs=[pr_run(fork_head, 60)],
    )
    save_state(base)
    done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
    check("a fork's run without the label is not planned", not planned and 'waits for' in done.stdout, done.stdout)
    for who, at, why in (
        ('eve', '2026-10-16T11:00:00Z', 'by someone not of the team'),
        ('bob', '2026-10-16T09:00:00Z', 'before its last push'),
    ):
        save_state(dict(base, pulls=[pull(9, fork_head, True, ['publish builds'])], events={'9': [labeled(who, at)]}))
        done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
        check(f'a label {why} lets nothing through', not planned, done.stdout)
    approved = [labeled('bob', '2026-10-16T11:00:00Z')]
    save_state(dict(base, pulls=[pull(9, fork_head, True, ['publish builds'])], events={'9': approved}))
    done, planned = run('plan', event={'workflow_run': pr_run(fork_head, 60)})
    check("the team's label after the push lets it through", planned.get('pr') == '9', done.stdout + done.stderr)
    event = {
        'action': 'labeled',
        'label': {'name': 'publish builds'},
        'pull_request': pull(9, fork_head, True, ['publish builds']),
    }
    done, planned = run('plan', name='pull_request_target', event=event)
    check('the label put on a finished run publishes it', planned.get('run') == '60', done.stdout + done.stderr)
    save_state(dict(state(), runs=[]))
    done, planned = run('plan', name='pull_request_target', event=event)
    check(
        'the label put while it builds waits for the run',
        not planned and 'has not ended' in done.stdout,
        done.stdout,
    )
    log_file.unlink(missing_ok=True)
    done, planned = run('plan', name='pull_request_target', event=dict(event, action='synchronize'))
    took = [c for c in calls() if c['args'][:3] == ['api', '-X', 'DELETE']]
    check(
        "a fork's push takes the label off, with the repository's token",
        took
        and took[0]['token'] == 'repo-token'
        and took[0]['args'][3].endswith('/labels/publish%20builds')
        and not state()['pulls'][0]['labels'],
        took,
    )

    # ---- no token for the nightly repository: it fails, saying which
    done, _ = run(
        'plan',
        name='pull_request_target',
        event={'action': 'closed', 'pull_request': closed},
        env_extra={'NIGHTLY_TOKEN': ''},
    )
    check(
        'without NIGHTLY_TOKEN it stops, naming it',
        done.returncode != 0 and 'NIGHTLY_TOKEN is not set' in done.stderr,
        done.stdout + done.stderr,
    )

    # ---- no webhook: published, and said
    save_state(dict(state(), pulls=[pull(8, head1)]))
    unhooked = ['--run', '52', '--sha', head1, '--pr', '8', '--artifacts', str(artifacts('pr1', head1))]
    done = publish('pr', *unhooked, hooks=False)
    check(
        'without a webhook it publishes and says nothing was posted',
        done.returncode == 0 and '::notice::No DISCORD_WEBHOOK_PRS' in done.stdout and find('pr-8'),
        done.stdout + done.stderr,
    )

server.shutdown()
for failure in failures:
    print('FAIL:', failure)
print('publish-builds:', 'FAILED' if failures else 'ok')
sys.exit(1 if failures else 0)
