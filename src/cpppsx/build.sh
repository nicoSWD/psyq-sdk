#!/bin/bash
# Build a native cpppsx (PsyQ's GNU cccp 2.8.1 preprocessor) for macOS / Linux.
# The GPL source is fetched from nocato/homebrew-psyq at a pinned commit; nothing is vendored here.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
work="$root/build/cpppsx"
rm -rf "$work" && mkdir -p "$work"
git clone -q https://github.com/nocato/homebrew-psyq.git "$work/src"
git -C "$work/src" checkout -q bdee891005a0aec6bf6fe40e504cf068ebf38caf
g="$work/src/gcc-2.8.1_psyq-4.4/gcc"
cp "$here/config.h" "$work/config.h"

# The PsyQ headers have CRLF line endings; the original cpppsx.exe read files in Windows text mode, which hides the CR.
python3 - "$g/cccp.c" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
pat = "  if ((fp->length > 0 && fp->buf[fp->length - 1] != '\\n')"
fix = ("  { long _i, _j;\n"
       "    for (_i = _j = 0; _i < fp->length; _i++) {\n"
       "      if (fp->buf[_i] == '\\r' && _i + 1 < fp->length && fp->buf[_i + 1] == '\\n') continue;\n"
       "      fp->buf[_j++] = fp->buf[_i];\n"
       "    }\n"
       "    fp->length = _j; }\n")
assert t.count(pat) == 2, t.count(pat)
t = t.replace(pat, fix + pat)
open(p, 'w').write(t)
PY
cd "$work"
bison -o cexp.c "$g/cexp.y"
f="-std=gnu89 -w -O1 -I. -I$g -I$g/config -I$g/config/mips -DHAVE_CONFIG_H"
cc $f -c "$g/cccp.c" -o cccp.o
cc $f -c cexp.c -o cexp.o
cc $f -c "$g/version.c" -o version.o
cc $f -DPREFIX=\"/usr/local\" -c "$g/prefix.c" -o prefix.o
cc $f -c "$g/obstack.c" -o obstack.o
mkdir -p "$root/bin"
cc -o "$root/bin/cpppsx" cccp.o cexp.o version.o prefix.o obstack.o
echo "built $root/bin/cpppsx"
