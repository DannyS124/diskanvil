#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later
"""What one tool got back that another didn't, from a benchmark results file.

    gap.py <results>.json [<tool that missed> [<tool that got it>]]

Defaults to DiskAnvil against Autopsy (The Sleuth Kit and PhotoRec together). For each image:
the test files the second tool got back whole (or with their name) and the first didn't, by kind.
"""

import collections
import json
import os
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    results = json.load(open(sys.argv[1]))
    a = sys.argv[2] if len(sys.argv) > 2 else 'diskanvil'
    b = sys.argv[3] if len(sys.argv) > 3 else 'autopsy'
    for image, row in results['images'].items():
        if a not in row or b not in row:
            continue
        missed_a = {m['file']: m['got'] for m in row[a]['missed']}
        missed_b = {m['file'] for m in row[b]['missed']}
        gap = [f for f in missed_a if f not in missed_b]
        ahead = [f for f in missed_b if f not in missed_a]
        if not gap and not ahead:
            continue
        print(f'{image}: {b} got {len(gap)} that {a} missed, {a} got {len(ahead)} that {b} missed')
        kinds = collections.Counter(os.path.splitext(f)[1].lower() or '(none)' for f in gap)
        if kinds:
            print('   missed by ' + a + ': ' + ', '.join(f'{k} {n}' for k, n in kinds.most_common()))
        kinds = collections.Counter(os.path.splitext(f)[1].lower() or '(none)' for f in ahead)
        if kinds:
            print('   missed by ' + b + ': ' + ', '.join(f'{k} {n}' for k, n in kinds.most_common()))


if __name__ == '__main__':
    main()
