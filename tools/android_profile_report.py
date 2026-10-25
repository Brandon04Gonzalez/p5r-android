#!/usr/bin/env python3
"""Report of a profile.txt sampled on Android (runtime/src/kernel_watch.cpp: touch <data root>/profile): samples per
thread, and for the busiest thread (or --tid) the libraries its samples are in and the hottest functions of our code
(first frame in libwonder.so of each sample), symbolized with the NDK's llvm-symbolizer against the unstripped
library (private/wonder/ba/libwonder.so).

usage: android_profile_report.py <profile.txt> [--tid N] [--top 40]
"""
import argparse
import collections
import glob
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, 'private/wonder/ba/libwonder.so')


def symbolizer():
    found = glob.glob(os.path.expanduser('~/Library/Android/sdk/ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-symbolizer'))
    return sorted(found)[-1] if found else 'llvm-symbolizer'


def symbolize(pcs):
    if not pcs:
        return {}
    out = subprocess.run([symbolizer(), '--obj=' + LIB, '--functions=short', '--no-inlines'],
                         input='\n'.join('0x' + p for p in pcs), capture_output=True, text=True).stdout
    blocks = [b.split('\n') for b in out.strip().split('\n\n')]
    names = {}
    for pc, b in zip(pcs, blocks):
        loc = b[1].replace(ROOT + '/', '') if len(b) > 1 else ''
        names[pc] = f'{b[0]}  {loc}'
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--tid')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    rows = [l.split() for l in open(a.profile) if l[:1].isdigit()]
    per_tid = collections.Counter(r[0] for r in rows)
    print('samples per thread:', ', '.join(f'{t}={n}' for t, n in per_tid.most_common(8)))
    tid = a.tid or per_tid.most_common(1)[0][0]
    mine = [r for r in rows if r[0] == tid]
    print(f'\nthread {tid}: {len(mine)} samples')
    libs = collections.Counter(r[2].split(':')[0] for r in mine if len(r) > 2)
    print('top frame by library:', ', '.join(f'{l}={n} ({100 * n / len(mine):.0f}%)' for l, n in libs.most_common(8)))
    first_self = collections.Counter()
    for r in mine:
        for f in r[2:]:
            if f.startswith('self:'):
                first_self[f[5:]] += 1
                break
    names = symbolize([pc for pc, _ in first_self.most_common(a.top)])
    print(f'\nhottest places in our code (first libwonder frame of each sample, {sum(first_self.values())} samples):')
    for pc, n in first_self.most_common(a.top):
        print(f'{n:5d} {100 * n / len(mine):5.1f}%  {names.get(pc, pc)}')


if __name__ == '__main__':
    main()
