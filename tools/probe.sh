#!/bin/bash
# Run the ORIGINAL aspsx on a small test file and print the object it writes, one record per line.
#
# usage: PSYQ_ORACLE_HOST=user@host PSYQ_ORACLE_SDK=/path/to/psyq_sdk probe.sh file.s [4.4|2.81|2.56]
#
# The oracle host is a machine that can run the Windows tools: `wibo` (or wine) must be on its PATH.
# The input is sent with CRLF line endings, which is what cc1psx writes and aspsx expects.
set -e
: "${PSYQ_ORACLE_HOST:?set PSYQ_ORACLE_HOST to a host that can run the original tools}"
SDK=${PSYQ_ORACLE_SDK:-\$HOME/psyq_sdk}
RUN=${PSYQ_ORACLE_RUNNER:-wibo}
VER=${2:-4.4}
case $VER in
  4.4)  EXE="$SDK/psyq_4.4/bin/aspsx.exe"; FL='-q -G 8';;
  2.81) EXE="$SDK/aspsx/2.81/aspsx.exe";   FL='-q -G 8';;
  2.56) EXE="$SDK/aspsx/2.56/aspsx.exe";   FL='-q';;
  *) echo "unknown version $VER" >&2; exit 2;;
esac
D=$(cd "$(dirname "$0")" && pwd)
sed 's/\r$//; s/$/\r/' "$1" | ssh "$PSYQ_ORACLE_HOST" "cat > /tmp/probe.s"
ssh "$PSYQ_ORACLE_HOST" "cd /tmp && rm -f probe.obj && $RUN $EXE $FL probe.s -o probe.obj >/tmp/probe.log 2>&1; cat /tmp/probe.log; cat probe.obj" > /tmp/probe.out
python3 -I - "$D" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import lnkdump
d = open('/tmp/probe.out', 'rb').read()
i = d.find(b'LNK\x02')
if i < 0:
    print(d.decode('latin-1')); sys.exit(1)
if i:
    print(d[:i].decode('latin-1'))
for off, r in lnkdump.records(d[i:]):
    if r.startswith(('SECTION', 'CPU')):
        continue
    print(r)
PY
