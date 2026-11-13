/*
 * aspsx - native re-implementation of the SN Systems ASPSX MIPS assembler
 * front end used by the PsyQ SDK (cc1psx .s -> SN "LNK" object).
 *
 * Clean-room: behaviour is derived from observing real input/output pairs,
 * not from the original code.
 *
 * usage: aspsx [-q] [-G n] input.s -o output.obj
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ util */

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#include <stdarg.h>
static int g_lineno;
static const char *g_infile = "";

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s(%d) : Error : ", g_infile, g_lineno);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("Out of memory, assembler aborting");
    return p;
}

static char *xstrdup(const char *s)
{
    char *r = xrealloc(NULL, strlen(s) + 1);
    strcpy(r, s);
    return r;
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static void bput(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b8(Buf *b, unsigned v) { uint8_t c = v; bput(b, &c, 1); }
static void b16(Buf *b, unsigned v) { b8(b, v & 255); b8(b, (v >> 8) & 255); }
static void b32(Buf *b, uint32_t v) { b16(b, v & 0xffff); b16(b, v >> 16); }
static void bstr(Buf *b, const char *s) { b8(b, (unsigned)strlen(s)); bput(b, s, strlen(s)); }

/* ---------------------------------------------------------------- tables */

enum { S_RDATA = 1, S_TEXT, S_DATA, S_SDATA, S_SBSS, S_BSS, S_CTORS, S_DTORS, NSECT = 8 };
static const char *const sect_names[NSECT + 1] = {
    0, ".rdata", ".text", ".data", ".sdata", ".sbss", ".bss", ".ctors", ".dtors"};

enum { REL_WORD32 = 16, REL_JUMP = 74, REL_HI16 = 82, REL_LO16 = 84, REL_GPREL = 100 };

typedef struct Sym {
    char *name;
    int id;          /* LNK symbol number, 0 = not numbered yet */
    int defined;
    int sect;
    uint32_t off;
    int global;      /* .globl */
    int is_extern;   /* .extern / .comm declaration */
    long size;       /* declared size for extern / comm / lcomm, -1 unknown */
    int is_ent;      /* declared with .ent */
    int is_comm;     /* .comm */
    int is_lcomm;    /* .lcomm */
    int def_seen;    /* pass 2: definition already passed (affects reloc expression shape) */
    int referenced;
    struct Sym *next; /* hash chain */
} Sym;

#define HASH_SIZE 256
static Sym *g_hash[HASH_SIZE];
static int g_nextid = 10;
static int g_gsize = 8; /* -G */
static uint32_t g_lc[NSECT + 1]; /* running size of the deferred .lcomm regions (.sbss, .bss) */

static unsigned name_hash(const char *s)
{
    unsigned h = 0;
    for (; *s; s++) h += (unsigned char)*s;
    return h & 255;
}

static Sym *sym_find(const char *name)
{
    for (Sym *s = g_hash[name_hash(name)]; s; s = s->next)
        if (!strcmp(s->name, name)) return s;
    return NULL;
}

static Sym *sym_get(const char *name)
{
    Sym *s = sym_find(name);
    if (s) return s;
    s = xrealloc(NULL, sizeof *s);
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    s->size = -1;
    unsigned h = name_hash(name);
    /* append to the end of the chain (insertion order) */
    Sym **pp = &g_hash[h];
    while (*pp) pp = &(*pp)->next;
    *pp = s;
    return s;
}

static void sym_number(Sym *s)
{
    if (!s->id) s->id = g_nextid++;
}

/* ------------------------------------------------------------ statements */

typedef struct {
    char *label;     /* label defined on this line, or NULL */
    char *op;        /* mnemonic / directive, or NULL */
    char **args;
    int nargs;
    int line;
} Stmt;

static Stmt *g_stmts;
static size_t g_nstmts, g_capstmts;

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int is_idchar(int c) { return isalnum(c) || c == '_' || c == '$' || c == '.'; }

/* split operand list on top level commas (respect () and "") */
static void split_args(char *s, char ***out, int *n)
{
    char **a = NULL;
    int cnt = 0, depth = 0, inq = 0;
    char *start = s;
    for (char *p = s;; p++) {
        if (*p == '"' && (p == s || p[-1] != '\\')) inq = !inq;
        if (!inq) {
            if (*p == '(') depth++;
            if (*p == ')') depth--;
        }
        if ((*p == ',' && !inq && depth == 0) || *p == 0) {
            char save = *p;
            *p = 0;
            a = xrealloc(a, sizeof(char *) * (cnt + 1));
            a[cnt++] = xstrdup(trim(start));
            if (!save) break;
            start = p + 1;
        }
    }
    if (cnt == 1 && !*a[0]) cnt = 0;
    *out = a;
    *n = cnt;
}

static void add_stmt(const char *label, const char *op, char *argstr, int line)
{
    if (g_nstmts == g_capstmts) {
        g_capstmts = g_capstmts ? g_capstmts * 2 : 1024;
        g_stmts = xrealloc(g_stmts, g_capstmts * sizeof(Stmt));
    }
    Stmt *st = &g_stmts[g_nstmts++];
    memset(st, 0, sizeof *st);
    st->line = line;
    if (label) st->label = xstrdup(label);
    if (op) {
        st->op = xstrdup(op);
        if (argstr) split_args(argstr, &st->args, &st->nargs);
    }
}

/* strip '#' comments that are outside of string literals */
static void strip_comment(char *line)
{
    int inq = 0;
    for (char *p = line; *p; p++) {
        if (*p == '"' && (p == line || p[-1] != '\\')) inq = !inq;
        if (*p == '#' && !inq) { *p = 0; return; }
    }
}

static void parse_line(char *line, int lineno)
{
    strip_comment(line);
    /* statements are separated by ';' (outside strings) */
    char *p = line;
    int inq = 0;
    char *stmt = p;
    for (;; p++) {
        if (*p == '"' && (p == line || p[-1] != '\\')) inq = !inq;
        if ((*p == ';' && !inq) || *p == 0) {
            char save = *p;
            *p = 0;
            char *s = trim(stmt);
            while (*s) {
                /* optional label */
                char *q = s;
                while (is_idchar((unsigned char)*q)) q++;
                if (q > s && *q == ':') {
                    char lab[256];
                    size_t len = (size_t)(q - s);
                    if (len >= sizeof lab) die("label too long");
                    memcpy(lab, s, len);
                    lab[len] = 0;
                    s = trim(q + 1);
                    if (!*s) { add_stmt(lab, NULL, NULL, lineno); break; }
                    /* label followed by statement: emit label statement first */
                    add_stmt(lab, NULL, NULL, lineno);
                    continue;
                }
                char *e = s;
                while (*e && !isspace((unsigned char)*e)) e++;
                char opb[64];
                size_t ol = (size_t)(e - s);
                if (ol >= sizeof opb) die("Op-code not recognised");
                memcpy(opb, s, ol);
                opb[ol] = 0;
                add_stmt(NULL, opb, trim(e), lineno);
                break;
            }
            if (!save) break;
            stmt = p + 1;
        }
    }
}

/* ----------------------------------------------------------- LNK writer */

static Buf g_out;          /* finished records */
static Buf g_chunk;        /* pending code bytes for the current section */
typedef struct {
    int type;
    unsigned at;
    Buf expr;
} Reloc;
static Reloc *g_relocs;
static size_t g_nrel, g_caprel;
static int g_cursect = S_TEXT;
static uint32_t g_loc[NSECT + 1];

static void flush_chunk(void)
{
    if (g_chunk.n) {
        b8(&g_out, 2);
        b16(&g_out, (unsigned)g_chunk.n);
        bput(&g_out, g_chunk.p, g_chunk.n);
        g_chunk.n = 0;
    }
    for (size_t i = 0; i < g_nrel; i++) {
        b8(&g_out, 10);
        b8(&g_out, (unsigned)g_relocs[i].type);
        b16(&g_out, g_relocs[i].at);
        bput(&g_out, g_relocs[i].expr.p, g_relocs[i].expr.n);
        free(g_relocs[i].expr.p);
    }
    g_nrel = 0;
}

static void do_switch(int sect)
{
    flush_chunk();
    b8(&g_out, 6);
    b16(&g_out, (unsigned)sect);
    g_cursect = sect;
}

static void emit_zeroes(uint32_t n)
{
    flush_chunk();
    b8(&g_out, 8);
    b32(&g_out, n);
    g_loc[g_cursect] += n;
}

static void emit_bytes(const void *d, size_t n)
{
    bput(&g_chunk, d, n);
    g_loc[g_cursect] += (uint32_t)n;
}

static void emit_word(uint32_t w)
{
    uint8_t c[4] = {w, w >> 8, w >> 16, w >> 24};
    emit_bytes(c, 4);
}

/* relocation expression builders */
static void expr_const(Buf *e, uint32_t v) { b8(e, 0); b32(e, v); }
static void expr_section(Buf *e, int sect) { b8(e, 4); b16(e, (unsigned)sect); }
static void expr_symbol(Buf *e, int id) { b8(e, 2); b16(e, (unsigned)id); }

/* expression for symbol + addend.  A symbol already defined earlier in the file folds the
 * addend into its offset; a forward reference (or an undefined symbol) keeps the addend as an
 * outer "constant + symbol" node. */
static void sym_expr(Buf *e, Sym *s, long addend)
{
    int fold = s->defined && s->def_seen;
    if (addend && !fold) {
        if (addend < 0) { b8(e, 46); expr_const(e, (uint32_t)(-addend)); }   /* sym - n */
        else { b8(e, 44); expr_const(e, (uint32_t)addend); }                 /* n + sym */
        addend = 0;
    }
    if (s->defined) {
        b8(e, 44); /* section + offset */
        expr_section(e, s->sect);
        expr_const(e, s->off + (uint32_t)addend);
    } else {
        sym_number(s);
        expr_symbol(e, s->id);
    }
}

static void add_reloc(int type, unsigned at_in_chunk, Sym *s, long addend)
{
    if (g_nrel == g_caprel) {
        g_caprel = g_caprel ? g_caprel * 2 : 64;
        g_relocs = xrealloc(g_relocs, g_caprel * sizeof(Reloc));
    }
    Reloc *r = &g_relocs[g_nrel++];
    memset(r, 0, sizeof *r);
    r->type = type;
    r->at = at_in_chunk;
    sym_expr(&r->expr, s, addend);
}

/* ----------------------------------------------------------- expressions */

typedef struct {
    Sym *sym;     /* NULL for a pure number */
    long value;   /* number or addend */
} Val;

static int parse_reg(const char *s)
{
    static const struct { const char *n; int r; } names[] = {
        {"zero", 0}, {"at", 1}, {"v0", 2}, {"v1", 3}, {"a0", 4}, {"a1", 5}, {"a2", 6}, {"a3", 7},
        {"t0", 8}, {"t1", 9}, {"t2", 10}, {"t3", 11}, {"t4", 12}, {"t5", 13}, {"t6", 14}, {"t7", 15},
        {"s0", 16}, {"s1", 17}, {"s2", 18}, {"s3", 19}, {"s4", 20}, {"s5", 21}, {"s6", 22}, {"s7", 23},
        {"t8", 24}, {"t9", 25}, {"k0", 26}, {"k1", 27}, {"gp", 28}, {"sp", 29}, {"fp", 30}, {"s8", 30},
        {"ra", 31}};
    if (*s != '$') return -1;
    s++;
    if (isdigit((unsigned char)*s)) {
        char *e;
        long v = strtol(s, &e, 10);
        if (*e || v < 0 || v > 31) return -1;
        return (int)v;
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(s, names[i].n)) return names[i].r;
    return -1;
}

/* parse "sym", "sym+4", "sym-4", "123", "-0x10", "0x1f" */
static int parse_val(const char *s, Val *v, int create)
{
    s = (const char *)trim((char *)s);
    v->sym = NULL;
    v->value = 0;
    if (!*s) return 0;
    const char *p = s;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;
    if (isdigit((unsigned char)*p)) {
        char *e;
        long n = strtol(p, &e, 0);
        if (*e) return 0;
        v->value = neg ? -n : n;
        return 1;
    }
    /* symbol with optional +/- number */
    char name[256];
    const char *q = s;
    while (is_idchar((unsigned char)*q)) q++;
    size_t len = (size_t)(q - s);
    if (!len || len >= sizeof name) return 0;
    memcpy(name, s, len);
    name[len] = 0;
    q = trim((char *)q);
    long add = 0;
    if (*q == '+' || *q == '-') {
        int sg = *q == '-';
        char *e;
        add = strtol(q + 1, &e, 0);
        if (*e) return 0;
        if (sg) add = -add;
    } else if (*q) {
        return 0;
    }
    v->sym = create ? sym_get(name) : sym_find(name);
    if (!v->sym) return 0;
    v->value = add;
    return 1;
}

/* --------------------------------------------------------------- passes */

static int g_pass;
static size_t g_idx;   /* index of the statement being processed (for one-statement lookahead) */
static int g_reorder = 1;
static int g_macro = 1;
static int g_noat;

static const char *numeric_label_name(const char *name, char *buf, size_t n);

static void define_label(const char *name_in)
{
    char nbuf[64];
    const char *name = numeric_label_name(name_in, nbuf, sizeof nbuf);
    Sym *s = sym_get(name);
    if (g_pass == 1) {
        if (s->defined) die("Label '%s' multiply defined", name);
        s->defined = 1;
        s->sect = g_cursect;
        s->off = g_loc[g_cursect];
    } else {
        /* sanity: pass 2 must see the same offset */
        if (s->off != g_loc[g_cursect] || s->sect != g_cursect)
            die("internal error (patch 1) label %s moved", name);
        s->def_seen = 1;
    }
}

static int strval_escape(const char *s, Buf *out)
{
    /* s points after the opening quote; returns chars consumed incl. closing quote */
    const char *p = s;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': b8(out, '\n'); p++; break;
            case 't': b8(out, '\t'); p++; break;
            case 'r': b8(out, '\r'); p++; break;
            case 'b': b8(out, '\b'); p++; break;
            case 'f': b8(out, '\f'); p++; break;
            case 'v': b8(out, '\v'); p++; break;
            case 'a': b8(out, 7); p++; break;
            case '\\': b8(out, '\\'); p++; break;
            case '"': b8(out, '"'); p++; break;
            case 'x': {
                p++;
                unsigned v = 0;
                while (isxdigit((unsigned char)*p)) {
                    v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10));
                    p++;
                }
                b8(out, v);
                break;
            }
            default:
                if (*p >= '0' && *p <= '7') {
                    unsigned v = 0;
                    int n = 0;
                    while (n < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (unsigned)(*p - '0'); p++; n++; }
                    b8(out, v);
                } else {
                    b8(out, (unsigned char)*p);
                    if (*p) p++;
                }
            }
        } else {
            b8(out, (unsigned char)*p++);
        }
    }
    return (int)(p - s) + (*p == '"');
}

static int section_from_name(const char *n)
{
    for (int i = 1; i <= NSECT; i++)
        if (!strcmp(n, sect_names[i])) return i;
    return 0;
}

static void do_align(int pow)
{
    uint32_t a = 1u << pow;
    uint32_t pad = (a - (g_loc[g_cursect] & (a - 1))) & (a - 1);
    static const uint8_t z[64];
    while (pad) {
        uint32_t k = pad > sizeof z ? (uint32_t)sizeof z : pad;
        emit_bytes(z, k);
        pad -= k;
    }
}

static int is_small_sym(Sym *s)
{
    if (s->defined) return s->sect == S_SDATA || s->sect == S_SBSS;
    return s->is_comm && s->size >= 0 && s->size <= g_gsize;
}

#include "insn.inc"

static void directive(Stmt *st)
{
    const char *op = st->op;
    if (!strcmp(op, ".ent")) {
        if (st->nargs) sym_get(st->args[0])->is_ent = 1;
        return;
    }
    if (!strcmp(op, ".file") || !strcmp(op, ".end") ||
        !strcmp(op, ".frame") || !strcmp(op, ".mask") || !strcmp(op, ".fmask") ||
        !strcmp(op, ".type") || !strcmp(op, ".size") || !strcmp(op, ".ident") ||
        !strcmp(op, ".verstamp")) {
        return;
    }
    if (!strcmp(op, ".set")) {
        if (st->nargs < 1) return;
        const char *a = st->args[0];
        if (!strcmp(a, "reorder")) g_reorder = 1;
        else if (!strcmp(a, "noreorder")) g_reorder = 0;
        else if (!strcmp(a, "macro")) g_macro = 1;
        else if (!strcmp(a, "nomacro")) g_macro = 0;
        else if (!strcmp(a, "at")) g_noat = 0;
        else if (!strcmp(a, "noat")) g_noat = 1;
        return;
    }
    if (!strcmp(op, ".text") || !strcmp(op, ".data") || !strcmp(op, ".rdata") ||
        !strcmp(op, ".sdata") || !strcmp(op, ".sbss") || !strcmp(op, ".bss") ||
        !strcmp(op, ".ctors") || !strcmp(op, ".dtors")) {
        if (g_pass == 2) do_switch(section_from_name(op));
        else { flush_chunk(); g_cursect = section_from_name(op); }
        return;
    }
    if (!strcmp(op, ".section")) {
        int s = st->nargs ? section_from_name(st->args[0]) : 0;
        if (!s) die("Illegal section name");
        if (g_pass == 2) do_switch(s);
        else g_cursect = s;
        return;
    }
    if (!strcmp(op, ".align")) {
        long n = st->nargs ? strtol(st->args[0], NULL, 0) : 0;
        if (g_pass == 1) g_loc[g_cursect] += 0, (void)n;
        do_align((int)n);
        return;
    }
    if (!strcmp(op, ".globl") || !strcmp(op, ".global")) {
        for (int i = 0; i < st->nargs; i++) {
            Sym *s = sym_get(st->args[i]);
            s->global = 1;
            if (g_pass == 1) sym_number(s);
        }
        return;
    }
    if (!strcmp(op, ".extern")) {
        Sym *s = sym_get(st->args[0]);
        s->is_extern = 1;
        if (st->nargs > 1) s->size = strtol(st->args[1], NULL, 0);
        if (g_pass == 1) sym_number(s);
        return;
    }
    if (!strcmp(op, ".byte") || !strcmp(op, ".half") || !strcmp(op, ".word")) {
        int w = !strcmp(op, ".byte") ? 1 : !strcmp(op, ".half") ? 2 : 4;
        for (int i = 0; i < st->nargs; i++) {
            Val v;
            if (!parse_val(st->args[i], &v, 1)) die("Error in expression");
            uint32_t x = (uint32_t)v.value;
            if (v.sym && w == 4) {
                if (g_pass == 2) add_reloc(REL_WORD32, (unsigned)g_chunk.n, v.sym, v.value);
                x = 0;
            }
            if (g_pass == 1) { g_loc[g_cursect] += (uint32_t)w; continue; }
            uint8_t c[4] = {x, x >> 8, x >> 16, x >> 24};
            emit_bytes(c, (size_t)w);
        }
        return;
    }
    if (!strcmp(op, ".space")) {
        long n = st->nargs ? strtol(st->args[0], NULL, 0) : 0;
        if (g_pass == 1) g_loc[g_cursect] += (uint32_t)n;
        else emit_zeroes((uint32_t)n);
        return;
    }
    if (!strcmp(op, ".ascii") || !strcmp(op, ".asciiz")) {
        Buf tmp = {0};
        for (int i = 0; i < st->nargs; i++) {
            const char *a = st->args[i];
            if (*a != '"') die("Missing string opernad in .ascii statement");
            strval_escape(a + 1, &tmp);
        }
        if (!strcmp(op, ".asciiz")) b8(&tmp, 0);
        if (g_pass == 1) g_loc[g_cursect] += (uint32_t)tmp.n;
        else emit_bytes(tmp.p, tmp.n);
        free(tmp.p);
        return;
    }
    if (!strcmp(op, ".comm") || !strcmp(op, ".lcomm")) {
        Sym *s = sym_get(st->args[0]);
        s->size = st->nargs > 1 ? strtol(st->args[1], NULL, 0) : 0;
        if (!strcmp(op, ".lcomm")) {
            if (g_pass == 1) {
                /* .lcomm storage lives in its own region, emitted once at the end of the file */
                int small = s->size <= g_gsize;
                int sect = small ? S_SBSS : S_BSS;
                uint32_t cap = small ? 8 : 16, al = 1;
                while (al < (uint32_t)s->size && al < cap) al <<= 1;   /* align to size, capped */
                uint32_t off = (g_lc[sect] + al - 1) & ~(al - 1);
                s->defined = 1;
                s->is_lcomm = 1;
                s->sect = sect;
                s->off = off;
                g_lc[sect] = off + (uint32_t)s->size;
            } else {
                s->def_seen = 1;
            }
        } else {
            s->is_extern = 1;
            s->is_comm = 1;
            if (g_pass == 1) sym_number(s);
        }
        return;
    }
    die("Op-code not recognised");
}

static void run_pass(int pass)
{
    g_pass = pass;
    if (pass == 1) memset(g_lc, 0, sizeof g_lc);
    g_cursect = S_TEXT;
    g_reorder = 1;
    g_macro = 1;
    g_noat = 0;
    memset(g_loc, 0, sizeof g_loc);
    insn_reset();
    for (size_t i = 0; i < g_nstmts; i++) {
        Stmt *st = &g_stmts[i];
        g_idx = i;
        g_lineno = st->line;
        /* A CODE record has a 16-bit length (and 16-bit relocation offsets): like the original, start a new
           record once a lot of code is pending.  Only statement boundaries are used. */
        if (pass == 2 && g_chunk.n >= 30000) flush_chunk();
        if (st->label) define_label(st->label);
        if (!st->op) continue;
        if (st->op[0] == '.') directive(st);
        else insn(st);
    }
}

static int cmp_sym_emit(const void *a, const void *b)
{
    (void)a; (void)b;
    return 0;
}

static void write_symbols(void)
{
    (void)cmp_sym_emit;
    for (int h = 0; h < HASH_SIZE; h++) {
        for (Sym *s = g_hash[h]; s; s = s->next) {
            if (s->defined && !s->global && (s->is_ent || s->is_lcomm)) {
                b8(&g_out, 18);
                b16(&g_out, (unsigned)s->sect);
                b32(&g_out, s->off);
                bstr(&g_out, s->name);
                continue;
            }
            if (!s->id) continue;
            if (s->global && s->defined) {
                b8(&g_out, 12);
                b16(&g_out, (unsigned)s->id);
                b16(&g_out, (unsigned)s->sect);
                b32(&g_out, s->off);
                bstr(&g_out, s->name);
            } else if (!s->defined && s->is_comm) {
                b8(&g_out, 48);
                b16(&g_out, (unsigned)s->id);
                b16(&g_out, s->size <= g_gsize ? S_SBSS : S_BSS);
                b32(&g_out, (uint32_t)s->size);
                bstr(&g_out, s->name);
            } else if (!s->defined) {
                b8(&g_out, 14);
                b16(&g_out, (unsigned)s->id);
                bstr(&g_out, s->name);
            }
        }
    }
}

/* ".file 1 "../source/a/b.c"" -> Z:\HOME\SOURCE\A\B.C style name */
static char *file_record_name(const char *path)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    char full[PATH_MAX * 2];
    if (path[0] == '/') snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s/%s", cwd, path);
    /* normalise . and .. */
    char *parts[256];
    int np = 0;
    for (char *t = strtok(full, "/"); t; t = strtok(NULL, "/")) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        if (np < 256) parts[np++] = t;
    }
    char *res = xrealloc(NULL, 2 + PATH_MAX * 2);
    strcpy(res, "Z:");
    for (int i = 0; i < np; i++) {
        strcat(res, "\\");
        strcat(res, parts[i]);
    }
    for (char *c = res; *c; c++) *c = (char)toupper((unsigned char)*c);
    return res;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) continue;
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { out = argv[++i]; continue; }
        if (!strcmp(argv[i], "-G") && i + 1 < argc) { g_gsize = atoi(argv[++i]); continue; }
        if (argv[i][0] == '-') { fprintf(stderr, "Unknown option %s\n", argv[i]); return 1; }
        if (in) { fprintf(stderr, "Too many file names specified\n"); return 1; }
        in = argv[i];
    }
    if (!in || !out) { fprintf(stderr, "usage : aspsx [-option ...] inputfile\n"); return 1; }
    g_infile = in;

    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "Could not open file '%s'\n", in); return 1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int lineno = 0;
    char *filename = NULL;
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!filename) {
            char *t = trim(line);
            if (!strncmp(t, ".file", 5)) {
                char *q = strchr(t, '"');
                if (q) {
                    char *e = strchr(q + 1, '"');
                    if (e) { *e = 0; filename = xstrdup(q + 1); }
                }
            }
        }
        parse_line(line, lineno);
    }
    fclose(f);

    run_pass(1);
    flush_chunk(); g_out.n = 0;
    memset(g_loc, 0, sizeof g_loc);
    run_pass(2);
    flush_chunk();
    /* deferred .lcomm regions */
    if (g_lc[S_SBSS]) { do_switch(S_SBSS); emit_zeroes(g_lc[S_SBSS]); }
    if (g_lc[S_BSS]) { do_switch(S_BSS); emit_zeroes(g_lc[S_BSS]); }

    Buf o = {0};
    bput(&o, "LNK\x02", 4);
    b8(&o, 46); b8(&o, 7);
    for (int i = 1; i <= NSECT; i++) {
        b8(&o, 16); b16(&o, (unsigned)i); b16(&o, 0); b8(&o, 8); bstr(&o, sect_names[i]);
    }
    b8(&o, 28); b16(&o, 9); bstr(&o, file_record_name(filename ? filename : in));
    b8(&o, 6); b16(&o, S_TEXT);
    bput(&o, g_out.p, g_out.n);
    g_out.n = 0;
    do_switch(S_TEXT);
    bput(&o, g_out.p, g_out.n);
    g_out.n = 0;
    write_symbols();
    bput(&o, g_out.p, g_out.n);
    b8(&o, 0);

    FILE *fo = fopen(out, "wb");
    if (!fo) { fprintf(stderr, "Error creating output file\n"); return 1; }
    fwrite(o.p, 1, o.n, fo);
    fclose(fo);
    return 0;
}
