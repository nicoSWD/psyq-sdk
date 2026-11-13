#!/usr/bin/env python3
"""Run the original aspsx.exe over every aspsx job of a reference build and keep each input with the original output.

usage: make_golden.py --ninja BUILD.NINJA --sdk SDK_DIR --out DIR [--runner wibo]

Needs a machine that can run the Windows tools (wibo or wine), SDK_DIR laid out as a PsyQ SDK checkout
(psyq_4.4/bin/aspsx.exe, aspsx/2.56/aspsx.exe, aspsx/2.81/aspsx.exe) and a build.ninja whose rules are named
psyq_aspsx_assemble_44 / _2_56 / _2_81 with `gSize` as the per-file variable.  Copy DIR to corpus/aspsx/ afterwards.
"""
import argparse, os, re, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--ninja', required=True)
ap.add_argument('--sdk', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--runner', default='wibo')
a = ap.parse_args()

root = os.path.dirname(os.path.abspath(a.ninja))
exes = {
    'psyq_aspsx_assemble_44': ('psyq_4.4/bin/aspsx.exe', ['-q', '-G'], '4.4'),
    'psyq_aspsx_assemble_2_81': ('aspsx/2.81/aspsx.exe', ['-q', '-G'], '2.81'),
    'psyq_aspsx_assemble_2_56': ('aspsx/2.56/aspsx.exe', ['-q'], '2.56'),
}
env = dict(os.environ, WINEDEBUG='-all')

lines = open(a.ninja).read().replace('$\n', ' ').splitlines()
jobs = []
for i, line in enumerate(lines):
    m = re.match(r'build (\S+): (psyq_aspsx_assemble_\S+)\s+(\S+)', line)
    if m:
        g = '8'
        j = i + 1
        while j < len(lines) and lines[j].startswith(' '):          # per-file variables follow the build line
            mm = re.match(r'\s+gSize = (\S+)', lines[j])
            if mm:
                g = mm.group(1)
            j += 1
        jobs.append(m.groups() + (g,))

shutil.rmtree(a.out, ignore_errors=True)
os.makedirs(a.out)
ok = bad = 0
for obj, rule, asm, gsize in jobs:
    exe, flags, ver = exes[rule]
    flags = flags + [gsize] if flags[-1] == '-G' else flags
    src = os.path.normpath(os.path.join(root, asm))
    if not os.path.exists(src):
        continue
    name = '%s/%s' % (ver, asm.replace('../obj/', '').replace('/', '__'))
    os.makedirs(os.path.join(a.out, os.path.dirname(name)), exist_ok=True)
    inp = os.path.join(a.out, name)
    shutil.copy(src, inp)
    outp = inp[:-4] + '.golden.obj'
    open(os.path.join(a.out, 'gsize.txt'), 'a').write('%s %s\n' % (name, gsize))
    r = subprocess.run([a.runner, os.path.join(a.sdk, exe), *flags, inp, '-o', outp], env=env, capture_output=True, text=True)
    open(inp[:-4] + '.log', 'w').write(r.stdout + r.stderr)
    if r.returncode == 0 and os.path.exists(outp):
        ok += 1
    else:
        bad += 1
        print('FAIL', name, r.returncode, file=sys.stderr)
print('ok=%d bad=%d total=%d' % (ok, bad, len(jobs)))
