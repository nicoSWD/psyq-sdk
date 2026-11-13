#!/usr/bin/env python3
"""Compile every golden job with a candidate cc1psx and compare the assembler output byte for byte.

usage: check_cc1psx.py CC1 [--ver 4.4|4.3] [--filter S] [--show N] [--jobs N]
CC1 is bin/cc1psx or tools/cc1psx_wrapper.py
The manifest (corpus/cc1psx/manifest.txt) lists: rule, input, output, -G size, as run by the real build.
"""
import argparse, collections, concurrent.futures, difflib, os, subprocess, sys, tempfile

CORPUS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'corpus', 'cc1psx')


def run(cc1, line):
    rule, inp, out, g = line.split()
    src = os.path.join(CORPUS, inp.replace('../', '', 1))
    gold = os.path.join(CORPUS, out.replace('../', '', 1))
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, 'o.asm')
        r = subprocess.run([cc1, '-quiet', '-O2', '-G', g, '-g0', '-Wall', src, '-o', o], capture_output=True, text=True, timeout=300)
        if r.returncode != 0 or not os.path.exists(o):
            return out, 'crash', 'exit %d %s' % (r.returncode, (r.stderr or '')[-160:])
        mine = open(o, 'rb').read()
    want = open(gold, 'rb').read()
    if mine == want:
        return out, 'ok', None
    a = want.decode('latin-1').split('\r\n')
    b = mine.decode('latin-1').split('\r\n')
    d = [l for l in difflib.unified_diff(a, b, 'golden', 'mine', lineterm='', n=0) if not l.startswith(('---', '+++', '@@'))]
    kind = 'trailing-text' if (len(d) == 2 and d[0] == '+' and d[1] == '+\t.text') or d[:3] == ['+', '+\t.text'] else 'diff'
    return out, kind, '\n'.join(d[:8])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cc1')
    ap.add_argument('--ver', default='4.4')
    ap.add_argument('--filter', default='')
    ap.add_argument('--show', type=int, default=3)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()
    rule = 'cc1psx-' + a.ver
    lines = [l for l in open(os.path.join(CORPUS, 'manifest.txt')).read().split('\n') if l.startswith(rule) and a.filter in l]
    cc1 = os.path.abspath(a.cc1)
    res = collections.defaultdict(list)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for out, kind, info in ex.map(lambda l: run(cc1, l), lines):
            res[kind].append((out, info))
    print('%d/%d identical (cc1psx %s)' % (len(res['ok']), len(lines), a.ver))
    for kind, items in sorted(res.items(), key=lambda kv: -len(kv[1])):
        if kind == 'ok':
            continue
        print('\n%-14s %d' % (kind, len(items)))
        for out, info in items[:a.show]:
            print('  ' + os.path.basename(out))
            if info:
                print('    ' + info.replace('\n', '\n    '))
    return 0 if len(res['ok']) == len(lines) else 1


if __name__ == '__main__':
    sys.exit(main())
