#!/usr/bin/env python3
"""Add cache entries for every compiler output of a reference build (made with the real tools) that the native
compiler does not reproduce.

usage: cc1psx_refresh_cache.py REFERENCE_OBJ_DIR NATIVE_OBJ_DIR NATIVE_BUILD_NINJA [COMPILER_RULE_REGEX]
"""
import hashlib, os, re, sys

ref, mine, ninja = sys.argv[1:4]
rule_re = sys.argv[4] if len(sys.argv) > 4 else r'psyq_cc_4[34]'      # ninja rule names of the compiler jobs
cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'corpus', 'cc1psx_cache')
os.makedirs(cache, exist_ok=True)
text = open(ninja).read().replace('$\n', ' ')
lines = text.split('\n')
gsz = {}
for i, l in enumerate(lines):
    m = re.match(r'build (\S+\.asm): (?:%s)\s+(\S+)' % rule_re, l)
    if m:
        g = '8'
        j = i + 1
        while j < len(lines) and lines[j].startswith(' '):
            mm = re.match(r'\s+gSize = (\S+)', lines[j])
            if mm:
                g = mm.group(1)
            j += 1
        gsz[os.path.normpath(os.path.join(os.path.dirname(ninja), m.group(1)))] = (os.path.normpath(os.path.join(os.path.dirname(ninja), m.group(2))), g)
n = 0
for out, (inp, g) in gsz.items():
    rel = os.path.relpath(out, mine)
    r = os.path.join(ref, rel)
    if not os.path.exists(r) or not os.path.exists(out):
        continue
    want, got = open(r, 'rb').read(), open(out, 'rb').read()
    if want != got:
        text = b''.join(l for l in open(inp, 'rb').read().split(b'\n') if not l.lstrip().startswith(b'#'))
        name = os.path.basename(inp).split('.')[0].encode()
        k = hashlib.sha256(name + b'\0' + b''.join(text.split()) + b'\0G=' + g.encode()).hexdigest()
        open(os.path.join(cache, k + '.asm'), 'wb').write(want)
        n += 1
        print('cached', rel)
print(n, 'new entries')
