# Developer tools

Python (and one shell) helpers used to **produce reference data from the original tools**, to **check the
re-implementations against it**, and to **investigate** behaviour that is not understood yet. None of them is
needed to *use* the tools in `bin/`, with one exception: `cc1psx_wrapper.py`, an optional front end for `cc1psx`.

All scripts need Python 3.8 or newer and nothing outside the standard library. Run them from the repository root.

## Reference corpora

The checkers compare a re-implementation with output of the original Windows tools. That data is derived from
the original SDK and from commercial software, so it lives in `corpus/` (git-ignored) and has to be produced by
whoever has the SDK and a project to build. Layouts:

| Directory | Contents |
|---|---|
| `corpus/aspsx/<ver>/` | `<name>.asm` (the input), `<name>.golden.obj` (original output), `<name>.log`; `corpus/aspsx/gsize.txt` maps `<ver>/<name>.asm` to the `-G` size the job used. `<ver>` is `4.4` or `2.56`. |
| `corpus/asmpsx/` | `<name>.s`, `<name>.golden.obj`, `<name>.log` |
| `corpus/psylink/` | `obj/` (the object tree, `link<N>.txt` command files, `golden<N>.cpe`, `golden<N>.map`, and the original `file()` outputs), plus an empty `build/` directory the command files' relative paths are resolved from |
| `corpus/cc1psx/` | `manifest.txt` with lines `cc1psx-4.4\|cc1psx-4.3 <input> <output> <-G size>`, and the inputs / original outputs under `obj/` |
| `corpus/cc1psx_cache/` | compiler output cache, see `cc1psx_wrapper.py` |

## Producing reference data

The original tools run under [wibo](https://github.com/decompals/wibo) or Wine, so these scripts are meant for a
Linux machine that has one of them. Run them there and copy the result into `corpus/`.

### `make_golden.py`
Runs the original `aspsx.exe` (2.56, 2.77/"4.4" and 2.81 as found in a PsyQ SDK checkout) over every assembler job
listed in a project's `build.ninja` and stores each input next to the original output.

```
make_golden.py --ninja BUILD.NINJA --sdk SDK_DIR --out DIR [--runner wibo]
```
It expects the ninja rules `psyq_aspsx_assemble_44`, `_2_56`, `_2_81` and a per-file `gSize` variable (the `-G`
value of the job), and the SDK laid out as `psyq_4.4/bin/aspsx.exe`, `aspsx/2.56/aspsx.exe`, `aspsx/2.81/aspsx.exe`.

### `make_golden_asmpsx.py`
The same for `asmpsx.exe` (`asmpsx /l /q in,out`), for jobs in rule `psyq_asmpsx_assemble`. `asmpsx` treats a
leading `/` as an option, so it is run inside the output directory with relative file names.

```
make_golden_asmpsx.py --ninja BUILD.NINJA --sdk SDK_DIR --out DIR [--runner wibo]
```

### `probe.sh`
Runs the **original** `aspsx` on one small assembly file over ssh and prints the object it writes, one record per
line (using `lnkdump.py`). The quickest way to ask "what does the real assembler do with this instruction?".

```
PSYQ_ORACLE_HOST=user@host [PSYQ_ORACLE_SDK=/path/to/psyq_sdk] [PSYQ_ORACLE_RUNNER=wibo] probe.sh file.s [4.4|2.81|2.56]
```
The input is converted to CRLF line endings first, which is what the compiler writes and the assembler expects.

### `probe_link.py`
Assembles tiny test objects that reference chosen symbols, links them with the **original** `psylink` against real
libraries and prints which library members were pulled in, in link order. This is how the library selection
algorithm of the linker was worked out.

```
PSYQ_ORACLE_HOST=user@host PSYQ_SDK_LOCAL=/local/psyq_sdk probe_link.py libc2.lib,libapi.lib puts,sprintf
```
`PSYQ_SDK_LOCAL` is a local copy of the SDK, only used to read the member names from the libraries.

## Checking the re-implementations

All checkers print how many reference jobs are reproduced and group the failures by their first difference.

### `check_assembler.py`
Scores `bin/aspsx` or `bin/asmpsx` against `corpus/aspsx` / `corpus/asmpsx`. Outputs are compared as parsed
object records, so a difference is reported as the first record that differs. Differences that cannot affect a
link are counted separately as "identical modulo CODE chunking / symbol-record order" (the original splits long
code runs into several records at points that follow no rule found so far; `asmpsx` emits its trailing symbol
records in an order not reproduced).

```
check_assembler.py bin/aspsx  --ver 4.4        # or 2.56
check_assembler.py bin/asmpsx --ver asmpsx
options: --filter SUBSTR  --show N  --jobs N
```
The `FILE` record of an object contains the working directory the original ran in, so it is compared by kind only.

### `check_psylink.py`
Runs `bin/psylink` (and `bin/psylink_overlay_fopen_mod` for the `_rhs` jobs, on files pre-filled with `0x9e`
exactly like the original build does) over every link in `corpus/psylink`, in a scratch copy, and compares the CPE
image, every `file()` output and the section table of the map byte for byte.

```
PSYQ_SDK=/path/to/psyq_sdk check_psylink.py [--filter SUBSTR] [--jobs N] [--show N]
```
`PSYQ_SDK` supplies the libraries the links pull members from.

### `check_cc1psx.py`
Compiles every job in `corpus/cc1psx/manifest.txt` with the given compiler and compares the assembler output byte
for byte.

```
check_cc1psx.py bin/cc1psx [--ver 4.4|4.3] [--filter S] [--show N]
check_cc1psx.py tools/cc1psx_wrapper.py
```

### `wdiff.py`
When a checked job fails, shows where: diffs the instruction words of the original and the candidate output
(`.text` only) as a sequence diff and, if the candidate understands `ASPSX_TRACE=1`, the source line each differing
word came from.

```
wdiff.py bin/aspsx NAME [4.4|2.56]       # NAME is the corpus file stem
```

### `lnkdump.py`
Dumps an SN `LNK` v2 object as one text line per record (`SECTION`, `CODE`, `RELOC` with its expression tree,
`XDEF`, ...), or diffs two objects. Used by the checkers and by `probe.sh`, and handy on its own.

```
lnkdump.py file.obj
lnkdump.py a.obj b.obj      # prints a unified diff of the record streams
```

## The `cc1psx` front end and its cache

### `cc1psx_wrapper.py`
A drop-in for the `cc1psx.exe` command line (`-quiet -O2 -G N -g0 -Wall input -o output`) that first looks the job
up in a cache of output produced by the original compiler and only then runs `bin/cc1psx`. It exists for the jobs
the native compiler cannot reproduce: PsyQ 4.3 jobs (no native compiler) and a few 4.4 jobs that are scheduled
differently. Pass `--ver 4.3` for 4.3 jobs; an uncached 4.3 job fails instead of being compiled wrongly.

The cache lives in `corpus/cc1psx_cache/` (override with `PSYQ_CC1_CACHE`). An entry is keyed by the SHA-256 of
the source file name, the preprocessed tokens (whitespace and line markers ignored, so different preprocessors give
the same key) and the `-G` size, so an edited source never gets a stale answer.

### `cc1psx_make_cache.py`
Fills the cache from `corpus/cc1psx`: every job the native `bin/cc1psx` does not reproduce, and every 4.3 job.

### `cc1psx_refresh_cache.py`
Adds cache entries from a build made with the original tools, for sources that changed since the corpus was made.

```
cc1psx_refresh_cache.py REFERENCE_OBJ_DIR NATIVE_OBJ_DIR NATIVE_BUILD_NINJA [COMPILER_RULE_REGEX]
```
It compares every compiler output of the reference build with the one the native build produced and caches the
reference output wherever they differ. `COMPILER_RULE_REGEX` is the ninja rule name of the compiler jobs
(default `psyq_cc_4[34]`).

## Build helpers

### `fix_bison_tokens.py`
Used by `src/cc1psx/build.sh`. Modern bison writes the token list of the C parser as an `enum`, whose names clash
with `RETURN` and friends in GCC 2.8.1's `rtl.def`; the old bison the sources were written for used `#define`s.
This rewrites `c-parse.h` / `c-parse.c` accordingly.

```
fix_bison_tokens.py c-parse.h c-parse.c
```
