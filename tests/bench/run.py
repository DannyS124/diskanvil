#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later
"""The recovery benchmark: every tool on every test drive, scored the same way.

    run.py <work folder> [--tools diskanvil,tsk,photorec] [--images ntfs-deleted,...] [--note "what changed"]

The images come from makeimages.py. Tools:
  diskanvil  build/diskanvil-recoverbench (Find Lost Files: quick scan, deep scan, save all)
  tsk        The Sleuth Kit's tsk_recover -e (every file the file system lists, deleted ones too)
  photorec   PhotoRec in the localhost/bench-photorec container (by contents, the whole drive)
  autopsy    not run: The Sleuth Kit and PhotoRec scored together, which is what Autopsy does

Saves <work folder>/results/<date>-<commit>.json and prints a table, with DiskAnvil's change
since the last results file.
"""

import datetime
import glob
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def option(name, default=None):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default


def recover(tool, image, out):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    start = time.monotonic()
    if tool == 'diskanvil':
        build = os.environ.get('DISKANVIL_BUILD', os.path.join(REPO, 'build'))
        subprocess.run([os.path.join(build, 'diskanvil-recoverbench'), image, out], check=True, stdout=subprocess.DEVNULL)
    elif tool == 'tsk':
        subprocess.run(['tsk_recover', '-e', image, out], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif tool == 'photorec':
        subprocess.run(['podman', 'run', '--rm', '-v', f'{os.path.dirname(image)}:/img:ro,Z', '-v', f'{out}:/out:Z', 'localhost/bench-photorec',
                        'photorec', '/d', '/out/recup', '/cmd', f'/img/{os.path.basename(image)}',
                        'partition_none,fileopt,everything,enable,wholespace,search'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return round(time.monotonic() - start, 1)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    work = os.path.abspath(sys.argv[1])
    tools = option('--tools', 'diskanvil,tsk,photorec').split(',')
    wanted = option('--images')
    images = sorted(glob.glob(os.path.join(work, 'images', '*.img')))
    if wanted:
        images = [i for i in images if os.path.basename(i)[:-4] in wanted.split(',')]
    commit = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', '-C', REPO, 'status', '--porcelain', '--', 'src'], capture_output=True, text=True).stdout.strip()
    results = {'when': datetime.datetime.now().isoformat(timespec='seconds'), 'commit': commit + ('+changes' if dirty else ''),
               'note': option('--note', ''), 'images': {}}

    for image in images:
        name = os.path.basename(image)[:-4]
        listing = json.load(open(image + '.json'))
        row = results['images'][name] = {}
        for tool in tools:
            out = os.path.join(work, 'runs', tool, name)
            seconds = recover(tool, image, out)
            row[tool] = score.score(listing, [out])
            row[tool]['seconds'] = seconds
            print(f'{name:18} {tool:10} {seconds:6}s', flush=True)
        if 'tsk' in tools and 'photorec' in tools:
            row['autopsy'] = score.score(listing, [os.path.join(work, 'runs', t, name) for t in ('tsk', 'photorec')])

    os.makedirs(os.path.join(work, 'results'), exist_ok=True)
    previous = sorted(glob.glob(os.path.join(work, 'results', '*.json')))
    path = os.path.join(work, 'results', f'{datetime.datetime.now():%Y%m%d-%H%M%S}-{commit}.json')
    json.dump(results, open(path, 'w'), indent=1)
    last = json.load(open(previous[-1])) if previous else None

    def cell(r, state):
        s = r['states'].get(state)
        if not s:
            return ''
        return f'{s.get("full", 0)}+{s.get("partial", 0)}p/{s["files"]} n{s.get("named", 0)}'

    print(f'\nfull+partial/files, n = with its name. Saved {path}\n')
    print(f'{"image":18} {"tool":10} {"deleted":>18} {"formatted":>18} {"live":>18} {"junk":>6}')
    for name, row in results['images'].items():
        for tool, r in row.items():
            line = f'{name:18} {tool:10} {cell(r, "deleted"):>18} {cell(r, "formatted"):>18} {cell(r, "live"):>18} {r["junk"]:6}'
            if tool == 'diskanvil' and last and name in last['images'] and 'diskanvil' in last['images'][name]:
                before = last['images'][name]['diskanvil']['states']
                change = []
                for state, s in r['states'].items():
                    b = before.get(state, {})
                    d = (s.get('full', 0) - b.get('full', 0), s.get('partial', 0) - b.get('partial', 0), s.get('named', 0) - b.get('named', 0))
                    if any(d):
                        change.append(f'{state} {d[0]:+} full {d[1]:+} partial {d[2]:+} named')
                if change:
                    line += '   since last: ' + ', '.join(change)
            print(line)


if __name__ == '__main__':
    main()
