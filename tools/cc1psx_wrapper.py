#!/usr/bin/env python3
"""cc1psx entry point for the build: same command line as cc1psx.exe.

  cc1psx_wrapper.py [--ver 4.3|4.4] -quiet -O2 -G N -g0 -Wall input.c -o output.asm

Looks the job up in the compiler-output cache first (jobs the native compiler cannot reproduce byte for byte),
otherwise runs the native 4.4 compiler.  A 4.3 job that is not cached cannot be compiled natively.
"""
import hashlib, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.environ.get('PSYQ_CC1_CACHE') or os.path.join(HERE, '..', 'corpus', 'cc1psx_cache')
CC1 = os.path.join(HERE, '..', 'bin', 'cc1psx')

args = sys.argv[1:]
ver = '4.4'
if args and args[0] == '--ver':
    ver, args = args[1], args[2:]
g = args[args.index('-G') + 1] if '-G' in args else '8'
out = args[args.index('-o') + 1]
src = next(a for a in args if a.endswith(('.eucjp', '.c', '.i')) and a != out)
text = b''.join(l for l in open(src, 'rb').read().split(b'\n') if not l.lstrip().startswith(b'#'))
name = os.path.basename(src).split('.')[0].encode()
k = hashlib.sha256(name + b'\0' + b''.join(text.split()) + b'\0G=' + g.encode()).hexdigest()
cached = os.path.join(CACHE, k + '.asm')
if os.path.exists(cached):
    open(out, 'wb').write(open(cached, 'rb').read())
    sys.exit(0)
if ver == '4.3':
    sys.stderr.write('cc1psx 4.3: no native compiler for this file and no cached output (%s)\n' % src)
    sys.exit(1)
sys.exit(subprocess.call([CC1] + args))
