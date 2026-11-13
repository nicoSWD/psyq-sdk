#!/bin/sh
# Runs every tool in bin/ once on a tiny program, to catch a build that produced binaries that cannot run.
set -eu
cd "$(dirname "$0")/.."
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT

printf 'int add(int a, int b)\n{\n    return a + b;\n}\n' > "$t/a.c"
bin/cpppsx -undef -Dmips -D__psx__ "$t/a.c" "$t/a.i"
bin/cc1psx -quiet -O2 -G 0 -g0 -Wall "$t/a.i" -o "$t/a.s"
bin/aspsx -q -G 0 "$t/a.s" -o "$t/a.obj"
test -s "$t/a.obj"

printf '\tsection .text\n\txdef start\nstart:\n\tdw 0\n\tdw 0\n' > "$t/b.s"
( cd "$t" && "$OLDPWD/bin/asmpsx" /l /q b.s,b.obj && test -s b.obj )

printf '        org $80010000\n        include "b.obj"\ntext    group\n        section .text, text\n        regs pc=start\n' > "$t/link.txt"
( cd "$t" && "$OLDPWD/bin/psylink" /c /q "@link.txt",out.cpe,out.sym,out.map >/dev/null && test -s out.cpe \
  && "$OLDPWD/bin/cpe2exe" -CJ out.cpe out.exe && test -s out.exe )
echo "smoke test ok"
