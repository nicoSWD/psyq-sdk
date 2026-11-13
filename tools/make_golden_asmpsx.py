#!/usr/bin/env python3
"""Run the original asmpsx.exe over every asmpsx job of a reference build and keep each input with the original output.

usage: make_golden_asmpsx.py --ninja BUILD.NINJA --sdk SDK_DIR --out DIR [--runner wibo]

Needs a machine that can run the Windows tools (wibo or wine) and a build.ninja whose assembler rule is named
psyq_asmpsx_assemble (`asmpsx /l /q in,out`).  Copy DIR to corpus/asmpsx/ afterwards.
asmpsx reads a leading '/' as an option, so it is run in DIR with relative file names.
"""
import argparse, os, re, shutil, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--ninja', required=True)
ap.add_argument('--sdk', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--runner', default='wibo')
a = ap.parse_args()

root = os.path.dirname(os.path.abspath(a.ninja))
exe = os.path.join(os.path.abspath(a.sdk), 'psyq_4.4/bin/asmpsx.exe')
env = dict(os.environ, WINEDEBUG='-all')
text = open(a.ninja).read().replace('$\n', ' ')
jobs = [m.groups() for m in (re.match(r'build (\S+): psyq_asmpsx_assemble\s+(\S+)', l) for l in text.splitlines()) if m]

shutil.rmtree(a.out, ignore_errors=True)
os.makedirs(a.out)
ok = bad = 0
for obj, asm in jobs:
    src = os.path.normpath(os.path.join(root, asm))
    name = asm.replace('../asm/', '').replace('/', '__')
    inp = os.path.join(a.out, name)
    shutil.copy(src, inp)
    outp = os.path.basename(inp[:-2] + '.golden.obj')
    r = subprocess.run([a.runner, exe, '/l', '/q', '%s,%s' % (name, outp)], env=env, capture_output=True, text=True, cwd=a.out)
    open(inp[:-2] + '.log', 'w').write(r.stdout + r.stderr)
    if r.returncode == 0 and os.path.exists(os.path.join(a.out, outp)):
        ok += 1
    else:
        bad += 1
        print('FAIL', name, r.returncode, file=sys.stderr)
print('ok=%d bad=%d total=%d' % (ok, bad, len(jobs)))
