#!/bin/bash
# Build a native cc1psx (PsyQ 4.4 flavour of GCC 2.8.1, target mips-psx) for macOS / Linux, 64-bit hosts.
# The GPL source is fetched from nocato/homebrew-psyq at a pinned commit and patched in a build directory;
# nothing is vendored in this repository.
#
# usage: src/cc1psx/build.sh        ->  bin/cc1psx
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
work="$root/build/cc1psx"
sha=bdee891005a0aec6bf6fe40e504cf068ebf38caf

rm -rf "$work" && mkdir -p "$work"
git clone -q https://github.com/nocato/homebrew-psyq.git "$work/src"
git -C "$work/src" checkout -q "$sha"
g="$work/src/gcc-2.8.1_psyq-4.4/gcc"

# ---- patches for a modern 64-bit host --------------------------------------------------------------------
python3 - "$g" <<'PY'
import re, sys
g = sys.argv[1]

# 1. `*((T *) p)++ = x` (cast as lvalue) is not accepted by clang
p = g + '/obstack.h'
t = open(p).read()
t = re.sub(r'\*\(\((void \*\*|int \*)\)__o->next_free\)\+\+ = ([^;]*);',
           lambda m: '*((%s)__o->next_free) = %s; __o->next_free += sizeof(%s);' % (m.group(1), m.group(2), 'void *' if 'void' in m.group(1) else 'int'), t)
t = re.sub(r'\(\*\(\((void \*\*|int \*|char \*\*)\) \(h\)->next_free\)\+\+ = (\((?:void \*|int|char \*)\))\s*(\w+)\)',
           lambda m: '(*((%s) (h)->next_free) = %s %s, (h)->next_free += sizeof(%s))' % (
               m.group(1), m.group(2), m.group(3), {'void **': 'void *', 'int *': 'int', 'char **': 'char *'}[m.group(1)]), t)
open(p, 'w').write(t)

# 2. xmalloc is used without a declaration in the CRLF-conversion code (pointer truncated on LP64)
p = g + '/unix2dos.c'
t = open(p).read()
if 'extern char *xmalloc' not in t:
    t = t.replace('#include <unistd.h>\n', '#include <unistd.h>\nextern char *xmalloc ();\n', 1)
open(p, 'w').write(t)

# 3. K&R redeclarations that would conflict with the variadic prototypes in psx_compat.h
p = g + '/config/mips/mips.c'
t = open(p).read()
for fn in ('error', 'fatal', 'warning'):
    t = t.replace('extern void   %s ();\n' % fn, '')
open(p, 'w').write(t)

# 5. bison 3.x rejects `$$` in a mid-rule action that has no declared type (bison 2.3 on macOS accepts it)
p = g + '/c-parse.in'
t = open(p).read()
t = re.sub(r'\$\$ = (start_struct|start_enum)', r'$<ttype>$ = \1', t)
open(p, 'w').write(t)

# 4. File switching (data first, text second) only when the GP optimisation is on, as the real compiler does
p = g + '/config/mips/psx.h'
t = open(p).read()
t = t.replace('#define TARGET_FILE_SWITCHING (1)', '#define TARGET_FILE_SWITCHING (TARGET_GP_OPT)')
open(p, 'w').write(t)
PY

# ---- configure (pretend to be the 32-bit i686 host the compiler was built on) ------------------------------
( cd "$g" && autoconf >/dev/null 2>&1 && autoheader >/dev/null 2>&1 )
mkdir "$work/obj" && cd "$work/obj"
CC=clang CFLAGS="-O1 -std=gnu89 -w" "$g/configure" --build=i686-pc-linux --host=i686-pc-linux --target=mips-psx > configure.log 2>&1

# modern bison writes the token list as an enum, which clashes with RETURN in rtl.def; old bison used #defines
make -C . c-parse.c >/dev/null 2>&1 || true
python3 "$root/tools/fix_bison_tokens.py" "$g/c-parse.h" "$g/c-parse.c" 2>/dev/null || true
touch "$g/c-parse.c" "$g/c-parse.h"

make cc1 -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" CC=clang HOST_CC=clang \
     HOST_CFLAGS="-O1 -std=gnu89 -fcommon -w -Wno-everything" \
     CFLAGS="-O1 -g0 -std=gnu89 -fcommon -w -Wno-everything -include $here/psx_compat.h" > make.log 2>&1 \
  || { tail -20 make.log; exit 1; }
mkdir -p "$root/bin"
cp cc1 "$root/bin/cc1psx"
echo "built $root/bin/cc1psx"
