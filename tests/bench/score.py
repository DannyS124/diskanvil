#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later
"""Scores what a recovery tool got back from one benchmark image.

    score.py <image>.json <folder> [<folder>...]

Every file in the folders (several folders count as one tool: The Sleuth Kit and PhotoRec
together, the way Autopsy runs them) is compared with the image's list of test files:

  full     a recovered file is the test file, byte for byte
  partial  not that, but one recovered file has at least a quarter of the test file's 4 KiB
           blocks (blocks that are in no other file, so blank or repeated ones don't count)
  named    full, and under the test file's own name
  junk     recovered files with no block of any file that was ever on the image, at any
           512-byte step (a piece of a fragmented file starts wherever its piece started)

A tool's own reports (DiskAnvil's "What was saved.txt" and SHA256SUMS) aren't counted.

Prints JSON: the counts for each state the test files were left in (live, deleted, formatted).
"""

import collections
import hashlib
import json
import os
import sys

BLOCK = 4096
PARTIAL = 0.25


REPORTS = {'What was saved.txt', 'SHA256SUMS'}


def recovered_files(folders):
    for folder in folders:
        for root, _, names in os.walk(folder):
            for n in names:
                if n not in REPORTS:
                    yield os.path.join(root, n)


def any_known_block(path, known, zero):
    """Whether any 4 KiB of the file, starting at any 512-byte step, is a block of a known file."""
    with open(path, 'rb') as f:
        data = f.read()
    for shift in range(512, BLOCK, 512):
        for at in range(shift, len(data) - BLOCK + 1, BLOCK):
            b = hashlib.sha256(data[at:at + BLOCK]).hexdigest()[:16]
            if b in known and b != zero:
                return True
    return False


def fingerprint(path):
    blocks, whole = [], hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(BLOCK):
            whole.update(chunk)
            blocks.append(hashlib.sha256(chunk).hexdigest()[:16])
    return whole.hexdigest(), blocks


def score(listing, folders):
    files, others = listing['files'], listing['others']
    # Blocks that say which file they're from: in one test file only, and in nothing else.
    seen = collections.Counter()
    for f in files + others:
        for b in set(f['blocks']):
            seen[b] += 1
    zero = hashlib.sha256(bytes(BLOCK)).hexdigest()[:16]
    known = set(seen)

    by_sha = collections.defaultdict(list)  # sha256 -> recovered names
    by_block = collections.defaultdict(set)  # unique block -> recovered file numbers
    junk = count = 0
    for path in recovered_files(folders):
        try:
            whole, blocks = fingerprint(path)
        except OSError:
            continue
        number = count
        count += 1
        by_sha[whole].append(os.path.basename(path).lower())
        if not any(b in known and b != zero for b in blocks) and not any_known_block(path, known, zero):
            junk += 1
        for b in blocks:
            if seen.get(b) == 1 and b != zero:
                by_block[b].add(number)

    states = collections.defaultdict(lambda: collections.Counter())
    missed = []
    for f in files:
        s = states[f['state']]
        s['files'] += 1
        if f['sha256'] in by_sha:
            s['full'] += 1
            if f['name'].lower() in by_sha[f['sha256']]:
                s['named'] += 1
            continue
        unique = [b for b in set(f['blocks']) if seen.get(b) == 1 and b != zero]
        hits = collections.Counter()
        for b in unique:
            for n in by_block.get(b, ()):
                hits[n] += 1
        best = max(hits.values(), default=0)
        if unique and best >= PARTIAL * len(unique):
            s['partial'] += 1
            missed.append({'file': f'{f["folder"]}/{f["name"]}', 'state': f['state'], 'got': round(best / len(unique), 2)})
        else:
            missed.append({'file': f'{f["folder"]}/{f["name"]}', 'state': f['state'], 'got': round(best / len(unique), 2) if unique else 0})
    return {'recovered': count, 'junk': junk, 'states': {k: dict(v) for k, v in states.items()}, 'missed': missed}


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit('usage: score.py <image>.json <folder> [<folder>...]')
    print(json.dumps(score(json.load(open(sys.argv[1])), sys.argv[2:]), indent=1))
