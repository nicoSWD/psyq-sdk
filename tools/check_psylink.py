#!/usr/bin/env python3
"""Run psylink over every golden link job and compare the CPE and file() outputs byte for byte.

usage: PSYQ_SDK=/path/to/psyq_sdk check_psylink.py [--filter SUBSTR] [--jobs N]
Works on a scratch copy of corpus/psylink so the golden files are never overwritten.
"""
import argparse, concurrent.futures, glob, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', 'corpus', 'psylink')
SDK = os.environ.get('PSYQ_SDK')
if not SDK:
    sys.exit('set PSYQ_SDK to the directory holding psyq_4.4/lib (libraries the golden links use)')
TOOL = os.path.join(HERE, '..', 'bin', 'psylink')
TOOL_MOD = os.path.join(HERE, '..', 'bin', 'psylink_overlay_fopen_mod')


def job(cmdfile, work):
    name = os.path.basename(cmdfile)[len('link'):-4]                              # '', '_title_lhs'
    # per-job scratch tree: obj/ is shared read-only through a symlink except for the files we write
    root = os.path.join(work, 'job' + (name or '_main'))
    os.makedirs(os.path.join(root, 'build'))
    os.makedirs(os.path.join(root, 'obj'))
    for e in os.listdir(os.path.join(CORPUS, 'obj')):
        if e.endswith(('.bin', '.cpe', '.map', '.sym')):
            continue                                                      # outputs: must be regenerated
        os.symlink(os.path.join(CORPUS, 'obj', e), os.path.join(root, 'obj', e))
    cpe = os.path.join(root, 'obj', 'out%s.cpe' % name)
    text0 = open(cmdfile, errors='replace').read()
    rhs = name.endswith('_rhs')
    if rhs:
        # build.py pre-creates the output files with 0x9e filler for the fopen-mod linker
        for m in re.finditer(r'file\("([^"]+)"\)', text0):
            open(os.path.normpath(os.path.join(root, 'build', m.group(1).replace('\\', '/'))), 'wb').write(b'\x9e' * (1024 * 1024))
    r = subprocess.run([TOOL_MOD if rhs else TOOL, '/l', '../../psyq_sdk/psyq_4.4/lib', '/c', '/n', '4000', '/q', '/gp', '.sdata', '/m',
                        '"@../obj/link%s.txt",%s,../obj/out%s.sym,../obj/out%s.map' % (name, cpe, name, name)],
                       cwd=os.path.join(root, 'build'), capture_output=True, text=True)
    if r.returncode != 0:
        return name, ['crash: ' + (r.stderr.strip().splitlines() or ['?'])[-1][:120]]
    problems = []
    mine_map = os.path.join(root, 'obj', 'out%s.map' % name)
    gold_map = os.path.join(CORPUS, 'obj', 'golden%s.map' % name)
    if os.path.exists(mine_map) and os.path.exists(gold_map):
        a, b = map_rows(mine_map), map_rows(gold_map)
        if a != b:
            problems.append('map sections differ: ' + next(('mine %s vs gold %s' % (x[4] + ' ' + x[0] + '+' + x[2], y[4] + ' ' + y[0] + '+' + y[2]) for x, y in zip(a, b) if x != y), 'row count %d vs %d' % (len(a), len(b))))
    gold = os.path.join(CORPUS, 'obj', 'golden%s.cpe' % name)
    if not os.path.exists(cpe) or open(cpe, 'rb').read() != open(gold, 'rb').read():
        problems.append('cpe differs')
    text = open(cmdfile, errors='replace').read()
    for m in re.finditer(r'file\("([^"]+)"\)', text):
        rel = m.group(1).replace('\\', '/')
        out = os.path.normpath(os.path.join(root, 'build', rel))
        g = os.path.normpath(os.path.join(CORPUS, 'build', rel))
        if not os.path.exists(out):
            problems.append('missing ' + os.path.basename(rel))
        elif not os.path.exists(g) or open(out, 'rb').read() != open(g, 'rb').read():
            problems.append('bin differs ' + os.path.basename(rel))
    return name, problems


def map_rows(path):
    """Section table of a MAP file: [(start, stop, length, group, name)]"""
    rows = []
    for l in open(path, errors='replace').read().replace('\r', '').split('\n')[1:]:
        p = l.split()
        if len(p) == 6 and re.fullmatch(r'[0-9A-F]{8}', p[0]):
            rows.append((p[0], p[1], p[2], p[4], p[5]))
        elif not l.strip():
            break
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--filter', default='')
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--show', type=int, default=40)
    a = ap.parse_args()
    files = sorted(f for f in glob.glob(os.path.join(CORPUS, 'obj', 'link*.txt')) if a.filter in f)
    with tempfile.TemporaryDirectory() as work:
        os.symlink(SDK, os.path.join(work, 'psyq_sdk'))
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            res = list(ex.map(lambda f: job(f, work), files))
    ok = [n for n, p in res if not p]
    print('%d/%d links reproduce the CPE and overlay bins exactly' % (len(ok), len(res)))
    shown = 0
    for n, p in res:
        if p and shown < a.show:
            shown += 1
            print('  %-22s %s' % (n or '(main)', '; '.join(p)))
    return 0 if len(ok) == len(res) else 1


if __name__ == '__main__':
    sys.exit(main())
