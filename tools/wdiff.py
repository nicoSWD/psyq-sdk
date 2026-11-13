#!/usr/bin/env python3
"""Word-level diff of the .text code of golden vs candidate output, with source lines.

usage: wdiff.py CANDIDATE NAME [ver]        NAME is the corpus file stem, e.g. libdg__bound
"""
import difflib
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import lnkdump  # noqa: E402

root = os.path.join(os.path.dirname(__file__), '..', 'corpus', 'aspsx')
cand, name = sys.argv[1], sys.argv[2]
ver = sys.argv[3] if len(sys.argv) > 3 else '4.4'
asm = os.path.join(root, ver, name + '.asm')
flags = ['-q', '-G', '8'] if ver != '2.56' else ['-q']


def text_words(path_or_bytes):
    d = open(path_or_bytes, 'rb').read() if isinstance(path_or_bytes, str) else path_or_bytes
    cur = None
    out = bytearray()
    for _, r in lnkdump.records(d):
        t = r.split()
        if t[0] == 'SWITCH':
            cur = int(t[1])
        elif t[0] == 'CODE' and cur == 2:
            out.extend(bytes.fromhex(t[2]))
    return [out[i:i + 4].hex() for i in range(0, len(out), 4)]


env = dict(os.environ, ASPSX_TRACE='1')
r = subprocess.run([cand, *flags, asm, '-o', '/tmp/wdiff.obj'], capture_output=True, text=True, env=env)
trace = {}
for l in r.stderr.splitlines():
    p = l.split()
    if len(p) == 3 and p[0] == 'T':
        trace.setdefault(int(p[2], 16) // 4, int(p[1]))
src = open(asm, errors='replace').read().replace('\r', '').split('\n')
g = text_words(asm[:-4] + '.golden.obj')
m = text_words('/tmp/wdiff.obj')
sm = difflib.SequenceMatcher(None, g, m, autojunk=False)
shown = 0
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    if tag == 'equal':
        continue
    shown += 1
    print('%s: golden words %d-%d (%s) vs mine %d-%d (%s)' % (tag, i1, i2, ' '.join(g[i1:i2][:6]), j1, j2, ' '.join(m[j1:j2][:6])))
    ln = trace.get(j1) or trace.get(j1 - 1)
    if ln:
        for k in range(max(1, ln - 3), ln + 2):
            print('      %5d %s' % (k, src[k - 1].replace('\t', ' ')))
    if shown >= 6:
        break
