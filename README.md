# PsyQ PlayStation (PS1) SDK

A re-implementation of the command line tools of the **PsyQ** PlayStation (PS1) SDK, so that PS1 projects can be
built on current systems (macOS, including Apple Silicon, and Linux) without the original Windows binaries,
Wine or an emulator.

The tools take the same command lines as the originals and produce the same files, byte for byte on every
reference input they were checked against.

| Tool | Replaces | How | Checked against the original |
|---|---|---|---|
| `cpppsx` | `cpppsx.exe` (C preprocessor, GNU cccp 2.8.1) | built from the PsyQ-patched GCC source | all sampled inputs identical |
| `cc1psx` | `cc1psx.exe` 4.4 (GCC 2.8.1 SN) | built from the PsyQ-patched GCC source, with a few 64-bit fixes | 726 of 729 reference compile jobs identical |
| `aspsx` | `aspsx.exe` 2.56 / 2.77 (shipped as 4.4) | C | 688 of 688 jobs (5 differ only in how long code runs are split into records) |
| `asmpsx` | `asmpsx.exe` | C | 731 of 731 jobs |
| `psylink` | `psylink.exe` | C | 159 of 159 links: CPE image and every `file()` output |
| `psylink_overlay_fopen_mod` | the patched linker that writes over existing files | the same binary run under that name | included in the 159 links |
| `cpe2exe` | `cpe2exe.exe` | C | identical |

## Building

```
make
```

builds everything into `bin/`. `make standalone` builds only the tools that need nothing but a C compiler
(`aspsx`, `asmpsx`, `psylink`, `cpe2exe`). `cpppsx` and `cc1psx` are built from upstream GPL source that the build
scripts fetch from [nocato/homebrew-psyq](https://github.com/nocato/homebrew-psyq) at a pinned commit and patch
for a modern 64-bit host; nothing from that tree is copied into this repository. They need `git`, `bison`,
`autoconf`, `gperf` and `clang`.

## Using the tools

Call them exactly like the Windows originals:

```
bin/cpppsx -undef -I include -Dmips -D__psx__ input.c output.i
bin/cc1psx -quiet -O2 -G 8 -g0 -Wall input.i -o output.s
bin/aspsx  -q -G 8 output.s -o output.obj
bin/asmpsx /l /q input.s,output.obj
bin/psylink /l sdk/lib /c /q /m "@link.txt",out.cpe,out.sym,out.map
bin/cpe2exe -CJ out.cpe out.exe
```

Notes on behaviour that differs from the originals:

* `cpppsx` writes `\n` line endings; the original, running in Windows text mode, wrote `\r\n`.
* `psylink` does not write the `.sym` file, and the order of symbols with the same address in the `.map` is not
  reproduced (nothing in a build reads either).
* `cc1psx` is the 4.4 compiler. The 4.3 compiler (GCC 2.7.2.SN) has no source available; see below.

The PsyQ libraries and headers are not part of this repository: you need your own copy of the SDK.

## Known gaps

* **PsyQ 4.3 `cc1psx`** is not reproduced. `tools/cc1psx_wrapper.py` can serve its output from a cache for inputs
  that were compiled with the real compiler before, and falls back to the native 4.4 compiler otherwise.
* **Three reference files** are scheduled differently by the native 4.4 compiler (two independent loads swapped by
  the second scheduling pass). The same cache covers them.
* `aspsx` 2.81 is not verified; only 2.77 and 2.56 are.

## How it was checked

The behaviour of the original tools was learned from their input and output files and from analysis of the
binaries; none of their code is included. For each
tool a reference corpus was produced by running the original program on the jobs of a real project, and the
re-implementation is scored against it job by job. What was learned is written down in
[`docs/behaviour-notes.md`](docs/behaviour-notes.md); the scripts that produce and check the corpora are described
in [`tools/README.md`](tools/README.md). The corpora themselves are derived from commercial software and from the
original SDK and are **not** in this repository.

## Releases

Pushing a tag `vX.Y.Z` builds every tool on macOS (arm64, x86_64) and Linux (x86_64, arm64) and attaches one
`psyq-sdk-<version>-<os>-<arch>.tar.gz` per platform, plus SHA-256 sums, to a GitHub release. Each archive holds
`bin/` with all seven tools, the licence files and `THIRD_PARTY.md`.

## Licence

The code in this repository (`aspsx`, `asmpsx`, `psylink`, `cpe2exe`, the build scripts and the Python tools) is
released under the [MIT licence](LICENSE).

`cpppsx` and `cc1psx` are built from GNU GCC 2.8.1 source (with the PsyQ patches) and are therefore GPL-2.0-or-later.
The patches and build scripts in `src/cpppsx` and `src/cc1psx` are released under the same terms. Release archives
contain those two binaries; [`THIRD_PARTY.md`](THIRD_PARTY.md) names the exact source they are built from.

This project is not affiliated with Sony Interactive Entertainment or SN Systems. PlayStation and PsyQ are
trademarks of their respective owners. No original SDK code or binaries are included.
