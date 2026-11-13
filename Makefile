# Builds every tool into bin/.  Needs: a C compiler, make, python3 and, for the two tools that are built from
# upstream GPL source (cpppsx, cc1psx), also git, bison, autoconf, gperf and clang.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter

TOOLS = bin/aspsx bin/asmpsx bin/psylink bin/psylink_overlay_fopen_mod bin/cpe2exe
UPSTREAM = bin/cpppsx bin/cc1psx

all: $(TOOLS) $(UPSTREAM)

# the tools that need nothing but a C compiler
standalone: $(TOOLS)

bin:
	mkdir -p bin

bin/aspsx: src/aspsx/aspsx.c src/aspsx/insn.inc | bin
	$(CC) $(CFLAGS) -o $@ src/aspsx/aspsx.c

bin/asmpsx: src/asmpsx/asmpsx.c | bin
	$(CC) $(CFLAGS) -o $@ src/asmpsx/asmpsx.c

bin/psylink: src/psylink/psylink.c | bin
	$(CC) $(CFLAGS) -o $@ src/psylink/psylink.c

# psylink behaves as psylink_overlay_fopen_mod when it is run under that name
bin/psylink_overlay_fopen_mod: bin/psylink
	ln -sf psylink $@

bin/cpe2exe: src/cpe2exe/cpe2exe.c | bin
	$(CC) $(CFLAGS) -o $@ src/cpe2exe/cpe2exe.c

bin/cpppsx: src/cpppsx/build.sh src/cpppsx/config.h | bin
	src/cpppsx/build.sh

bin/cc1psx: src/cc1psx/build.sh src/cc1psx/psx_compat.h tools/fix_bison_tokens.py | bin
	src/cc1psx/build.sh

clean:
	rm -rf bin build

.PHONY: all standalone clean
