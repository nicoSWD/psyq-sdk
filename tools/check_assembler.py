#!/usr/bin/env python3
"""Score a candidate aspsx / asmpsx binary against the golden corpus.

usage: check_assembler.py CANDIDATE [--ver 4.4|2.56|asmpsx] [--filter SUBSTR] [--show N] [--jobs N]

For every corpus/aspsx/<ver>/*.asm the candidate is run with the same flags
build.py uses and its output is compared byte for byte with *.golden.obj.
Failures are summarised by the first differing record so the biggest bucket
can be fixed first.
"""
import argparse
import collections
import concurrent.futures
import glob
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import lnkdump  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), '..', 'corpus', 'aspsx')
FLAGS = {'4.4': ['-q', '-G', '8'], '2.81': ['-q', '-G', '8'], '2.56': ['-q']}
_GSIZE = {}
try:
    for _l in open(os.path.join(ROOT, 'gsize.txt')):
        _n, _g = _l.split()
        _GSIZE[_n] = _g
except OSError:
    pass


def merge_chunks(recs):
    """Merge CODE chunks that follow each other in one section (no SWITCH/ZEROES between):
    the linker sees identical content, only the record chunking differs."""
    out = []
    i = 0
    while i < len(recs):
        r = recs[i]
        if not r.startswith('CODE'):
            out.append(r)
            i += 1
            continue
        code = bytearray(bytes.fromhex(r.split()[2]))
        relocs = []
        j = i + 1
        while True:
            while j < len(recs) and recs[j].startswith('RELOC'):
                t = recs[j].split()
                at = int(t[2][3:], 16) + (len(code) - 0)
                relocs.append(recs[j])
                j += 1
            if j < len(recs) and recs[j].startswith('CODE'):
                base = len(code)
                code.extend(bytes.fromhex(recs[j].split()[2]))
                # relocs that followed this chunk are rebased below
                k = j + 1
                shifted = []
                while k < len(recs) and recs[k].startswith('RELOC'):
                    t = recs[k].split()
                    at = int(t[2][3:], 16) + base
                    shifted.append('RELOC %s at=%04x %s' % (t[1], at, ' '.join(t[3:])))
                    k += 1
                relocs.extend(shifted)
                j = k
            else:
                break
        out.append('CODE %d %s' % (len(code), code.hex()))
        out.extend(relocs)
        i = j
    return out


def run_one(cand, asm, ver):
    asmpsx = ver == 'asmpsx'
    golden = (asm[:-2] if asmpsx else asm[:-4]) + '.golden.obj'
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, 'out.obj')
        try:
            if asmpsx:
                # asmpsx reads a leading '/' as an option: pass a relative input, like build.py does
                r = subprocess.run([cand, '/l', '/q', '%s,%s' % (os.path.basename(asm), out)],
                                   capture_output=True, text=True, timeout=60, cwd=os.path.dirname(asm))
            else:
                flags = list(FLAGS[ver])
                key = ver + '/' + os.path.basename(asm)
                if '-G' in flags and key in _GSIZE:
                    flags[flags.index('-G') + 1] = _GSIZE[key]      # the build passes a per-file -G
                r = subprocess.run([cand, *flags, asm, '-o', out],
                                   capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            return asm, 'timeout', None
        if r.returncode != 0 or not os.path.exists(out):
            return asm, 'crash/exit %d' % r.returncode, (r.stdout + r.stderr)[-200:]
        got = open(out, 'rb').read()
    want = open(golden, 'rb').read()
    if got == want:
        return asm, 'ok', None
    try:
        # the FILE record embeds the cwd of the run, so compare it by kind only
        norm = lambda r: 'FILE' if r.startswith('FILE ') else r
        a = [norm(x) for _, x in lnkdump.records(want)]
        b = [norm(x) for _, x in lnkdump.records(got)]
    except Exception as e:  # malformed candidate output
        return asm, 'unparseable', str(e)
    if a == b:
        return asm, 'ok', None
    na, nb = merge_chunks(a), merge_chunks(b)
    if asmpsx:
        # asmpsx emits its symbol records in an order I could not derive from the single file that has
        # several symbols; every record carries its explicit symbol number, so compare them as a set
        key = lambda r: r.startswith(('XDEF', 'XREF', 'LOCAL', 'C48'))
        na = [r for r in na if not key(r)] + sorted(r for r in na if key(r))
        nb = [r for r in nb if not key(r)] + sorted(r for r in nb if key(r))
    if na == nb:
        return asm, 'ok-modulo-chunking', None
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            if x.startswith('CODE') and y.startswith('CODE'):
                xb, yb = bytes.fromhex(x.split()[2]), bytes.fromhex(y.split()[2])
                k = next((j for j in range(min(len(xb), len(yb))) if xb[j] != yb[j]), min(len(xb), len(yb)))
                k &= ~3
                return asm, 'diff:CODE', 'rec %d len want %d got %d, first diff at byte 0x%x\n  want %s | %s\n  got  %s | %s' % (
                    i, len(xb), len(yb), k, xb[max(0, k - 8):k].hex(), xb[k:k + 12].hex(), yb[max(0, k - 8):k].hex(), yb[k:k + 12].hex())
            return asm, 'diff:' + x.split()[0] + '/' + y.split()[0], 'rec %d\n  want %s\n  got  %s' % (i, x[:150], y[:150])
    return asm, 'length', 'want %d records, got %d' % (len(a), len(b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('candidate')
    ap.add_argument('--ver', default='4.4')
    ap.add_argument('--filter', default='')
    ap.add_argument('--show', type=int, default=3)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    a = ap.parse_args()

    files = sorted(f for f in (glob.glob(os.path.join(ROOT, '..', 'asmpsx', '*.s')) if a.ver == 'asmpsx'
                           else glob.glob(os.path.join(ROOT, a.ver, '*.asm'))) if a.filter in f)
    cand = os.path.abspath(a.candidate)
    buckets = collections.defaultdict(list)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for asm, status, info in ex.map(lambda f: run_one(cand, f, a.ver), files):
            buckets[status].append((asm, info))

    ok = len(buckets.get('ok', []))
    loose = len(buckets.get('ok-modulo-chunking', []))
    print('%d/%d byte-identical (%s); %d more identical modulo CODE chunking / symbol-record order' % (ok, len(files), a.ver, loose))
    for status, items in sorted(buckets.items(), key=lambda kv: -len(kv[1])):
        if status in ('ok', 'ok-modulo-chunking'):
            continue
        print('\n%-28s %d files' % (status, len(items)))
        for asm, info in items[:a.show]:
            print('  ' + os.path.basename(asm))
            if info:
                print('    ' + info.replace('\n', '\n    '))
    return 0 if ok + loose == len(files) else 1


if __name__ == '__main__':
    sys.exit(main())
