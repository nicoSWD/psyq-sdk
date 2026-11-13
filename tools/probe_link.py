#!/usr/bin/env python3
"""Ask the ORIGINAL psylink which library members it pulls, and in which order.

usage: PSYQ_ORACLE_HOST=user@host [PSYQ_ORACLE_SDK=/path/to/psyq_sdk] probe_link.py LIBS NAMES [NAMES ...]

  LIBS   comma separated library names from psyq_4.4/lib, in inclib order (e.g. libc2.lib,libapi.lib)
  NAMES  comma separated symbols; each NAMES group becomes one tiny test object that references them

The oracle host must be able to run the Windows tools (`wibo` or wine on its PATH).  Prints the pulled members
in link (address) order as lib#index(member).
"""
import base64, os, subprocess, sys

HOST = os.environ.get('PSYQ_ORACLE_HOST')
if not HOST:
    sys.exit('set PSYQ_ORACLE_HOST to a host that can run the original tools')
SDK = os.environ.get('PSYQ_ORACLE_SDK', '$HOME/psyq_sdk')
RUN = os.environ.get('PSYQ_ORACLE_RUNNER', 'wibo')
if len(sys.argv) < 3:
    sys.exit(__doc__)
libs = sys.argv[1].split(',')
groups = [g.split(',') for g in sys.argv[2:]]

files = {}
cmd = ['    org    $80010000']
for gi, names in enumerate(groups):
    src = '\topt\tc+, at+, e+, n-\n\tsection .text\n\txdef\tobj%d\n' % gi
    src += ''.join('\txref\t%s\n' % n for n in names)
    src += 'obj%d:\n' % gi + ''.join('\tjal\t%s\n\tnop\n' % n for n in names)
    files['t%d.s' % gi] = src
    cmd.append('    include "t%d.obj"' % gi)
cmd += ['    inclib "%s"' % l for l in libs]
cmd += ['text group', '    section .rdata, text', '    section .text,  text', '    section .data,  text', '    section .sdata, text',
        'bss group bss', '    section .sbss,  bss', '    section .bss,   bss', '    regs    pc=obj0']
files['cmd.txt'] = '\n'.join(cmd) + '\n'

work = '/tmp/psyq_probe'
sh = ['rm -rf %s && mkdir -p %s && cd %s' % (work, work, work), 'ln -s %s sdk' % SDK]
for name, content in files.items():
    sh.append('echo %s | base64 -d > %s' % (base64.b64encode(content.replace('\n', '\r\n').encode()).decode(), name))
for gi in range(len(groups)):
    sh.append('%s sdk/psyq_4.4/bin/asmpsx.exe /l /q t%d.s,t%d.obj >/dev/null 2>&1' % (RUN, gi, gi))
sh.append('%s sdk/psyq_4.4/bin/psylink.exe /l sdk/psyq_4.4/lib /c /q /m "@cmd.txt",o.cpe,o.sym,o.map >link.log 2>&1 || true' % RUN)
sh.append('cat link.log; cat o.map')
out = subprocess.run(['ssh', HOST, ' && '.join(sh[:-2]) + '; ' + '; '.join(sh[-2:])], capture_output=True).stdout.decode('latin-1').replace('\r', '')

sym = {}
for l in out.split('\n'):
    p = l.split()
    if len(p) == 2 and len(p[0]) == 8:
        try:
            sym[p[1]] = int(p[0], 16)
        except ValueError:
            pass

# member -> exported symbols, read from the local copies of the libraries
import struct
libdir = os.environ.get('PSYQ_SDK_LOCAL')
if not libdir:
    sys.exit('set PSYQ_SDK_LOCAL to a local copy of the SDK to name the members')
rows = []
for l in libs:
    d = open(os.path.join(libdir, 'psyq_4.4/lib', l), 'rb').read()
    p, mi = 4, 0
    while p + 20 <= len(d):
        hdr, total = struct.unpack_from('<II', d, p + 12)
        name = d[p:p + 8].decode('latin-1').strip()
        q, syms = p + 20, []
        while d[q] != 0:
            syms.append(d[q + 1:q + 1 + d[q]].decode('latin-1'))
            q += 1 + d[q]
        a = [sym[s] for s in syms if s in sym]
        if a:
            rows.append((min(a), '%s#%d(%s)' % (l.replace('.lib', ''), mi, name)))
        p += total
        mi += 1
print(' '.join(r[1] for r in sorted(rows)))
if not rows:
    print(out[-600:])
