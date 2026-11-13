# Behaviour notes: what the original tools do

Everything here was observed by running the original PsyQ tools (see `tools/README.md` for how the reference data
was produced) and comparing against these re-implementations. Nothing is taken from the original code.

# aspsx (assembler for `cc1psx` output)

Method: black-box. Inputs are cc1psx `.s` output, outputs are SN `LNK` v2 objects.
Everything below was measured on 688 real jobs (674 aspsx 4.4/"2.77", 14 aspsx 2.56).

## Output layout (aspsx 4.4, `-q -G 8`, `-g0` input)
`CPU 7`, then 8 fixed `SECTION` records (`.rdata .text .data .sdata .sbss .bss .ctors .dtors`, ids 1-8,
align 8), `FILE 9 <path>`, then `SWITCH`/`CODE`/`ZEROES` data, `RELOC`s,
`XDEF`s, `LOCAL`s/`XREF`s, `END`. Record tags seen in the whole corpus:
CPU SECTION FILE SWITCH CODE RELOC XDEF XREF LOCAL ZEROES C48 END.

* `FILE` path is the `.file` string resolved against the **current directory** of
  the process, upper-cased, `/` -> `\`, drive `Z:` (wine/wibo). It is environment dependent;
  the checker must ignore it.
* A file starts with an implicit `SWITCH 2` (.text) and ends with a `SWITCH 2`.
* `.space n` inside initialised data splits the `CODE` run and emits `ZEROES n`.

## Symbol record order
Symbol records (`XDEF`, `XREF`, `LOCAL`, `C48`) are emitted by walking a 256-bucket table whose hash is
`sum(bytes of name) & 255`, buckets in ascending order. Symbols in one bucket come out in the order they were first
mentioned in the input (a `.globl`, `.ent`, label or reference); this reproduces all reference objects exactly.

## About the original executables
`aspsx`, `asmpsx` and `psylink` are written largely in hand-tuned x86 assembly with register-based calling
conventions, global state and indirect jumps through function-pointer globals, so their disassembly is not a usable
basis for a re-implementation. Behaviour was learned from input/output pairs and from probing the originals with small
purpose-made inputs; a disassembler is only useful for targeted questions (hash functions, tables, constants).

## Rules established (all verified against the corpus)

Symbols
* `.globl` / `.extern` / `.comm` / `.lcomm` symbols are numbered first, in declaration order (from #10 when a `.file`
  record takes #9); symbols only referenced from code are numbered afterwards in order of first use.
* Final records are emitted by walking a 256-bucket hash table (bucket = byte sum of the name & 255): `XDEF`
  (global + defined), `XREF` (undefined), `C48` (`.comm`: id, section 5 or 6, size, name) and `LOCAL`
  (`.ent` non-global functions and every `.lcomm`).
* `.lcomm` storage is deferred: one `SWITCH 5 / ZEROES n` (size <= -G) and one `SWITCH 6 / ZEROES n` at the end of the
  file. Each symbol is aligned to next_pow2(size), capped at 8 (.sbss) / 16 (.bss) and does *not* advance the normal
  .bss location counter.
* `$gp`-relative (reloc 100) only for symbols defined in .sdata/.sbss and `.comm`/`.lcomm` of size <= -G.
  `.extern` symbols are never gp-relative, whatever their size.

Relocations
* hi16 = 82, lo16 = 84, jump26 = 74, word32 = 16, gprel16 = 100.
* Expression: defined symbol = `op44(op4(section), #offset)`, undefined = `op2(id)`.
  An addend folds into the offset only if the symbol was already defined earlier in the file; otherwise
  `op44(#add, expr)` (or `op46(#abs, expr)` for a negative addend).

Instructions / hazards
* `.set reorder` adds a delay-slot `nop` after branches and jumps; a load (lb lh lw lbu lhu lwl lwr mfc2 cfc2)
  whose destination is read by the next instruction gets a `nop`, decided by the *producer's* mode; `cfc2`
  always gets it. `lwl`/`lwr` pairs are exempt. The `div`/`rem` macro result counts as a load result, a plain
  `mflo`/`mfhi` does not.
* mflo/mfhi followed by mult/div/mthi/mtlo within two instructions: pad to two (also resolved before a noreorder
  branch whose delay slot holds such an instruction).
* A cop2 command needs two instructions after lwc2/mtc2/ctc2 (explicit nops count).
* Globals: `lui tmp,%hi ; op rt,%lo(tmp)`, tmp = the destination for plain GPR loads, `$at` otherwise;
  `sym(base)`: `lui $at; addu $at,$at,base; op rt,%lo($at)`. Absolute numbers outside 16 bits behave the same way.
* `la rt,number` == `li`. `sltu rd,rs,-n` goes through `$at`. 3-operand `div/rem[u]` expand to
  `div; bne; nop; break 7; [addiu/bne/lui/bne/nop/break 6]; mflo|mfhi`.
* `break N` (explicit) = `N << 6 | 13`; the macro breaks use `N << 16 | 13`.

Open
* In ~30 files the real tool splits one `CODE` record in two at points that follow no rule found so far (a record
  length is 16 bits, so long runs have to be split; this re-implementation does so every 30000 bytes at statement
  boundaries). The linker input is equivalent, so `tools/check_assembler.py` reports those as "identical modulo CODE chunking".

## asmpsx (separate tool, SN assembler syntax)
* Sections get ids in order of first `section` directive, starting at **2**; all `SECTION` records come first, then one
  `SWITCH` per `section` directive. No `FILE` record, no implicit initial `SWITCH`.
* Symbols (`xdef`/`xref`, declaration order) are numbered from `#sections + 2`; implicit references afterwards.
* No hazard handling at all. `@name` labels are scoped to the preceding global label.
* `sym>>16` -> reloc type 26, expr `op60(#16, base)`; `sym&0xffff[+k]` -> type 30, expr `op44(#k, op52(#ffff, base))`.
* Symbol record order is NOT the aspsx hash (byte sum & 255); no simple hash family reproduces it. Order is
  semantically irrelevant (explicit ids).

## psylink (src/psylink/psylink.c) - all 159 reference links match

Inputs: a command file (`org`, `include "x.obj"[, group]`, `inclib "x.lib"[, group]`, `<name> group [bss] [file("...")]`,
`section[.N] <name>, <group>`, `regs pc=<symbol>`; an `include` line may lack its closing quote, paths use backslashes),
`LNK` objects and `LIB` archives.

Output CPE: `CPE\x01 08 00 03 90 00 <pc u32> {01 addr len data}* 00` - one load chunk per object CODE record in link
order, ZEROES in loadable sections as zero chunks, nothing for the bss group (set register 0x90 is the PC).

Archives: `LIB\x01`, then entries of name[8], date u32, header length u32, entry length u32 (the entry length counts
the header), a symbol list (pascal strings, 0 terminated) and the `LNK` object.

Library selection (verified on all 161 members of one link and by probing the real tool): repeat { scan the `inclib`
libraries in order and each library's members in index order; pull the first member that exports a currently
undefined symbol; absorb its definitions and references immediately }. The order in which symbols were referenced
does not matter. Objects included `include "x.obj", GROUP` (and members pulled by `inclib "x.lib", GROUP`)
contribute their sections to `GROUP.<name>` (".text" -> "overlay.text"); references from them never change the main
image.

Layout:
* Sections of a group in `section` order; within a section the objects in `include` order, then pulled members.
  Contributions are 4-byte aligned, `section.N` sets N. The main `.bss` starts 16-aligned and aligns to 8.
* A section's end is padded to its alignment; a section whose contributions are all empty only has the alignment
  effect and no row in the map.
* Common symbols (`C48` records; library objects use section ids `0xF000|id`) are allocated right after the
  declaring object's own data, in hash-table order (byte sum of the name modulo 509, ties newest first). Each occupies
  whole words and is zero-filled in file output. A symbol defined by any object suppresses the common.

Symbols and relocations:
* Exported symbols: first definition wins. Linker-defined: `__<sect>_org/obj/orgend/objend/size` (size = sum of the
  contributions, without padding, commons included), `_<group>_org/...` for whole groups, `__SN_GP_BASE` = start of
  `.sdata`.
* Relocation types: 74 jump26, 82 hi16 (with carry), 84 lo16, 16 word32, 100 gp-relative (relative to
  `__SN_GP_BASE`), 26 / 30 store the low 16 bits of an already shifted / masked expression.
* Expression nodes: const, symbol id, section base, `x12(section)` = start and `x22(section)` = end of the linked
  section; operators 44 add, 46 `b-a`, 50 `b/a`, 52 `b&a`, 60 `b>>a` (operands are written constant first).

`file("...")` groups: the file is zero filled up to the end of the last chunk written (CODE of loadable sections,
ZEROES and commons of every section; padding CODE records inside bss are not written). The patched linker
(`psylink_overlay_fopen_mod`) writes the same chunks on top of an existing file instead, including the padding CODE
records of bss sections, and leaves every byte it does not write alone - the build pre-fills the files with 0x9e.

## cpe2exe (src/cpe2exe/cpe2exe.c)
PS-X EXE: 0x800 header (id, pc, gp, text addr, text size, initial sp 0x801FFFF0, region string) + image padded to 0x800.
