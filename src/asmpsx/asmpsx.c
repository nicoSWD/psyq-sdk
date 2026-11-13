/*
 * asmpsx - native re-implementation of the SN Systems ASMPSX assembler
 * (PsyQ SDK, SN syntax -> SN "LNK" object).
 *
 * Clean-room: behaviour is derived from observing real input/output pairs.
 *
 * usage: asmpsx [/l] [/q] input.s,output.obj
 *
 * Unlike aspsx this assembler does no hazard handling: every nop and delay
 * slot is explicit in the source.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ util */

static int g_lineno;
static const char *g_infile = "";

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
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
static void b8(Buf *b, unsigned v) { uint8_t c = (uint8_t)v; bput(b, &c, 1); }
static void b16(Buf *b, unsigned v) { b8(b, v & 255); b8(b, (v >> 8) & 255); }
static void b32(Buf *b, uint32_t v) { b16(b, v & 0xffff); b16(b, v >> 16); }
static void bstr(Buf *b, const char *s) { b8(b, (unsigned)strlen(s)); bput(b, s, strlen(s)); }

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* -------------------------------------------------------------- sections */

#define MAXSECT 16
typedef struct {
    char *name;
    int id;
    uint32_t loc;
} Section;

static Section g_sect[MAXSECT];
static int g_nsect;
static int g_cur = -1; /* index into g_sect */

static int sect_find(const char *name)
{
    for (int i = 0; i < g_nsect; i++)
        if (!strcmp(g_sect[i].name, name)) return i;
    return -1;
}

/* ----------------------------------------------------------------- symbols */

enum { REL_WORD32 = 16, REL_JUMP = 74, REL_HI16 = 82, REL_LO16 = 84, REL_GPREL = 100 };

typedef struct Sym {
    char *name;
    int id;
    int defined, sect; /* sect = section index */
    uint32_t off;
    int is_xdef, is_xref;
    int def_seen;
    struct Sym *next;
} Sym;

#define HASH_SIZE 256
static Sym *g_hash[HASH_SIZE];
static int g_nextid;

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

static int g_scope; /* '@' labels belong to the preceding global label */

static Sym *sym_get(const char *name_in)
{
    char scoped[320];
    const char *name = name_in;
    if (name_in[0] == '@') {
        snprintf(scoped, sizeof scoped, "%d%s", g_scope, name_in);
        name = scoped;
    }
    Sym *s = sym_find(name);
    if (s) return s;
    s = xrealloc(NULL, sizeof *s);
    memset(s, 0, sizeof *s);
    s->name = xstrdup(name);
    Sym **pp = &g_hash[name_hash(name)];
    while (*pp) pp = &(*pp)->next;
    *pp = s;
    return s;
}

static void sym_number(Sym *s)
{
    if (!s->id) s->id = g_nextid++;
}

/* --------------------------------------------------------------- statements */

typedef struct {
    char *label;
    char *op;
    char **args;
    int nargs;
    int line;
} Stmt;

static Stmt *g_stmts;
static size_t g_nstmts, g_capstmts;

static void split_args(char *s, char ***out, int *n)
{
    char **a = NULL;
    int cnt = 0, depth = 0;
    char quote = 0;
    char *start = s;
    for (char *p = s;; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            depth--;
        }
        if ((*p == ',' && !quote && depth == 0) || *p == 0) {
            char save = *p;
            *p = 0;
            a = xrealloc(a, sizeof(char *) * (size_t)(cnt + 1));
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
        g_capstmts = g_capstmts ? g_capstmts * 2 : 4096;
        g_stmts = xrealloc(g_stmts, g_capstmts * sizeof(Stmt));
    }
    Stmt *st = &g_stmts[g_nstmts++];
    memset(st, 0, sizeof *st);
    st->line = line;
    if (label) st->label = xstrdup(label);
    if (op) {
        char *o = xstrdup(op);
        for (char *c = o; *c; c++) *c = (char)tolower((unsigned char)*c);
        st->op = o;
        if (argstr) split_args(argstr, &st->args, &st->nargs);
    }
}

static int is_idch(int c) { return isalnum(c) || c == '_' || c == '@' || c == '.' || c == '$'; }

static void parse_line(char *line, int lineno)
{
    /* ';' starts a comment unless inside a character/string literal */
    char quote = 0;
    for (char *p = line; *p; p++) {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '\'' || *p == '"') quote = *p;
        else if (*p == ';') { *p = 0; break; }
    }
    char *s = trim(line);
    if (!*s) return;
    /* label: identifier at column 0 followed by ':' */
    if (!isspace((unsigned char)line[0])) {
        char *q = s;
        while (is_idch((unsigned char)*q)) q++;
        if (q > s && *q == ':') {
            *q = 0;
            add_stmt(s, NULL, NULL, lineno);
            s = trim(q + 1);
            if (!*s) return;
        }
    }
    char *e = s;
    while (*e && !isspace((unsigned char)*e)) e++;
    char save = *e;
    *e = 0;
    char *op = s;
    char *rest = save ? trim(e + 1) : e;
    add_stmt(NULL, op, rest, lineno);
}

enum { OP_NONE = 0, OP_SHR = 1, OP_AND = 2 };
typedef struct {
    Sym *sym;
    long value;   /* number, or symbol addend / outer addend */
    int op;       /* OP_SHR: sym >> arg ; OP_AND: sym & arg */
    long arg;
} Val;

/* ------------------------------------------------------------- LNK writer */

static Buf g_out;    /* body records */
static Buf g_chunk;  /* pending code bytes */
typedef struct {
    int type;
    unsigned at;
    Buf expr;
} Reloc;
static Reloc *g_relocs;
static size_t g_nrel, g_caprel;
static int g_pass;

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

static void do_switch(int idx)
{
    flush_chunk();
    b8(&g_out, 6);
    b16(&g_out, (unsigned)g_sect[idx].id);
    g_cur = idx;
}

static void emit_bytes(const void *d, size_t n)
{
    if (g_cur < 0) die("Section not specified");
    if (g_pass == 2) {
        /* a CODE record cannot exceed 16 bits of length */
        if (g_chunk.n + n > 0xffff) flush_chunk();
        bput(&g_chunk, d, n);
    }
    g_sect[g_cur].loc += (uint32_t)n;
}

static void emit_word(uint32_t w)
{
    uint8_t c[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24)};
    emit_bytes(c, 4);
}

static void expr_const(Buf *e, uint32_t v) { b8(e, 0); b32(e, v); }

static void sym_expr(Buf *e, Sym *s, long addend)
{
    int fold = s->defined && s->def_seen;
    if (addend && !fold) {
        if (addend < 0) { b8(e, 46); expr_const(e, (uint32_t)(-addend)); }
        else { b8(e, 44); expr_const(e, (uint32_t)addend); }
        addend = 0;
    }
    if (s->defined) {
        b8(e, 44);
        b8(e, 4); b16(e, (unsigned)g_sect[s->sect].id);
        expr_const(e, s->off + (uint32_t)addend);
    } else {
        sym_number(s);
        b8(e, 2); b16(e, (unsigned)s->id);
    }
}

static void add_reloc_v(int type, const Val *v);
static void add_reloc(int type, Sym *s, long addend)
{
    Val v = {s, addend, OP_NONE, 0};
    add_reloc_v(type, &v);
}

static void add_reloc_v(int type, const Val *v)
{
    if (g_pass != 2) return;
    if (g_nrel == g_caprel) {
        g_caprel = g_caprel ? g_caprel * 2 : 64;
        g_relocs = xrealloc(g_relocs, g_caprel * sizeof(Reloc));
    }
    Reloc *r = &g_relocs[g_nrel++];
    memset(r, 0, sizeof *r);
    r->type = type;
    r->at = (unsigned)g_chunk.n;
    if (v->op == OP_SHR) {
        b8(&r->expr, 60); expr_const(&r->expr, (uint32_t)v->arg);
        sym_expr(&r->expr, v->sym, 0);
    } else if (v->op == OP_AND) {
        if (v->value) { b8(&r->expr, 44); expr_const(&r->expr, (uint32_t)v->value); }
        b8(&r->expr, 52); expr_const(&r->expr, (uint32_t)v->arg);
        sym_expr(&r->expr, v->sym, 0);
    } else {
        sym_expr(&r->expr, v->sym, v->value);
    }
}

/* ------------------------------------------------------------ expressions */



static int parse_reg(const char *s)
{
    static const struct { const char *n; int r; } names[] = {
        {"zero", 0}, {"at", 1}, {"v0", 2}, {"v1", 3}, {"a0", 4}, {"a1", 5}, {"a2", 6}, {"a3", 7},
        {"t0", 8}, {"t1", 9}, {"t2", 10}, {"t3", 11}, {"t4", 12}, {"t5", 13}, {"t6", 14}, {"t7", 15},
        {"s0", 16}, {"s1", 17}, {"s2", 18}, {"s3", 19}, {"s4", 20}, {"s5", 21}, {"s6", 22}, {"s7", 23},
        {"t8", 24}, {"t9", 25}, {"k0", 26}, {"k1", 27}, {"gp", 28}, {"sp", 29}, {"fp", 30}, {"s8", 30},
        {"ra", 31}};
    const char *p = s;
    if (*p == '$') p++;
    if (isdigit((unsigned char)*p) && p != s) {
        char *e;
        long v = strtol(p, &e, 10);
        return (!*e && v >= 0 && v <= 31) ? (int)v : -1;
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(p, names[i].n)) return names[i].r;
    return -1;
}

/* number: 0x1F, $1F, 123, 'A' */
static int parse_num(const char *s, long *out)
{
    s = trim((char *)s);
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    char *e;
    long v;
    if (s[0] == '\'' && s[1] && s[2] == '\'' && !s[3]) { v = (unsigned char)s[1]; }
    else if (s[0] == '$') { v = (long)strtoul(s + 1, &e, 16); if (*e || !s[1]) return 0; }
    else if (isdigit((unsigned char)s[0])) { v = (long)strtoul(s, &e, 0); if (*e) return 0; }
    else return 0;
    *out = neg ? -v : v;
    return 1;
}

/* index of the ')' matching the '(' at s[0], or -1 */
static int match_paren(const char *s)
{
    int d = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '(') d++;
        else if (s[i] == ')' && --d == 0) return i;
    }
    return -1;
}

/* value: number | sym | sym+n | sym-n | sym>>n | sym&mask | (expr) | (expr)+n */
static int parse_val(const char *in, Val *v)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", in);
    char *s = trim(buf);
    v->sym = NULL; v->value = 0; v->op = OP_NONE; v->arg = 0;
    if (!*s) return 0;
    if (*s == '(') {
        int m = match_paren(s);
        if (m < 0) return 0;
        char *rest = trim(s + m + 1);
        s[m] = 0;
        char inner[256];
        snprintf(inner, sizeof inner, "%s", s + 1);
        if (!*rest) return parse_val(inner, v);
        if (*rest == '+' || *rest == '-') {
            long n;
            if (!parse_num(rest + 1, &n)) return 0;
            if (!parse_val(inner, v)) return 0;
            v->value += *rest == '-' ? -n : n;
            return 1;
        }
        return 0;
    }
    if (parse_num(s, &v->value)) return 1;
    char *opp = strstr(s, ">>");
    int op = OP_SHR;
    if (!opp) { opp = strchr(s, '&'); op = OP_AND; }
    char name[256];
    if (opp) {
        long n;
        char *rhs = op == OP_SHR ? opp + 2 : opp + 1;
        if (!parse_num(rhs, &n)) return 0;
        *opp = 0;
        char *lhs = trim(s);
        snprintf(name, sizeof name, "%s", lhs);
        v->sym = sym_get(name);
        v->op = op;
        v->arg = n;
        return 1;
    }
    const char *q = s;
    while (is_idch((unsigned char)*q)) q++;
    size_t len = (size_t)(q - s);
    if (!len || len >= sizeof name) return 0;
    memcpy(name, s, len);
    name[len] = 0;
    q = trim((char *)q);
    long add = 0;
    if (*q == '+' || *q == '-') {
        long n;
        if (!parse_num(q + 1, &n)) return 0;
        add = *q == '-' ? -n : n;
    } else if (*q) {
        return 0;
    }
    v->sym = sym_get(name);
    v->value = add;
    return 1;
}

/* ------------------------------------------------------------ instructions */

#define R(rs, rt, rd, sh, fn) \
    ((uint32_t)(((rs) & 31) << 21 | ((rt) & 31) << 16 | ((rd) & 31) << 11 | ((sh) & 31) << 6 | (fn)))
#define I(op, rs, rt, imm) ((uint32_t)((op) << 26 | ((rs) & 31) << 21 | ((rt) & 31) << 16 | ((imm) & 0xffff)))

static void put(uint32_t w, int rel, Sym *s, long add)
{
    if (rel) add_reloc(rel, s, add);
    emit_word(w);
}

static int reg_arg(Stmt *st, int i)
{
    if (i >= st->nargs) die("Unexpected end of line");
    int r = parse_reg(st->args[i]);
    if (r < 0) die("Illegal register");
    return r;
}

static long imm_arg(Stmt *st, int i)
{
    Val v;
    if (i >= st->nargs || !parse_val(st->args[i], &v) || v.sym) die("Syntax error in expression");
    return v.value;
}

typedef struct {
    int base;
    Val v;
    int has_sym;
} Mem;

static void parse_mem(const char *a, Mem *m)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", a);
    m->base = -1;
    size_t l = strlen(tmp);
    char *lp = NULL;
    if (l && tmp[l - 1] == ')') {
        lp = strrchr(tmp, '(');
        if (lp) {
            char inner[256];
            snprintf(inner, sizeof inner, "%.*s", (int)(l - 1 - (size_t)(lp + 1 - tmp)), lp + 1);
            int r = parse_reg(trim(inner));
            if (r >= 0) { m->base = r; *lp = 0; }
        }
    }
    const char *t = trim(tmp);
    if (!*t) { m->v.sym = NULL; m->v.value = 0; m->has_sym = 0; return; }
    if (!parse_val(t, &m->v)) die("Syntax error in expression");
    m->has_sym = m->v.sym != NULL;
}

static void mem_op(unsigned opc, int rt, Mem *m)
{
    if (m->has_sym && m->v.op == OP_AND) {
        /* disp = (sym & mask) [+ k](base) : raw 16-bit field with a type-30 relocation */
        add_reloc_v(30, &m->v);
        emit_word(I(opc, m->base < 0 ? 0 : m->base, rt, 0));
        return;
    }
    if (!m->has_sym) {
        put(I(opc, m->base < 0 ? 0 : m->base, rt, (uint32_t)m->v.value), 0, NULL, 0);
        return;
    }
    Sym *s = m->v.sym;
    int tmp = (opc == 32 || opc == 33 || opc == 35 || opc == 36 || opc == 37) ? rt : 1;
    put(I(15, 0, tmp, 0), REL_HI16, s, m->v.value);
    if (m->base >= 0) put(R(tmp, m->base, tmp, 0, 33), 0, NULL, 0);
    put(I(opc, tmp, rt, 0), REL_LO16, s, m->v.value);
}

static void branch(uint32_t base, const char *target)
{
    Val v;
    if (!parse_val(target, &v) || !v.sym) die("Syntax error in expression");
    Sym *s = v.sym;
    if (g_pass == 2) {
        if (!s->defined || s->sect != g_cur) die("Branch to undefined or foreign label '%s'", target);
        long off = ((long)s->off - ((long)g_sect[g_cur].loc + 4)) >> 2;
        base = (base & 0xffff0000u) | ((uint32_t)off & 0xffff);
    }
    emit_word(base);
}

static void insn(Stmt *st)
{
    const char *op = st->op;
    int n = st->nargs;

    static const struct { const char *n; unsigned fn; } alu3[] = {
        {"add", 32}, {"addu", 33}, {"sub", 34}, {"subu", 35}, {"and", 36}, {"or", 37},
        {"xor", 38}, {"nor", 39}, {"slt", 42}, {"sltu", 43}};
    for (size_t i = 0; i < sizeof alu3 / sizeof *alu3; i++)
        if (!strcmp(op, alu3[i].n)) {
            put(R(reg_arg(st, 1), reg_arg(st, 2), reg_arg(st, 0), 0, alu3[i].fn), 0, NULL, 0);
            return;
        }
    static const struct { const char *n; unsigned fn; } shi[] = {{"sll", 0}, {"srl", 2}, {"sra", 3}};
    for (size_t i = 0; i < sizeof shi / sizeof *shi; i++)
        if (!strcmp(op, shi[i].n)) {
            put(R(0, reg_arg(st, 1), reg_arg(st, 0), (unsigned)imm_arg(st, 2), shi[i].fn), 0, NULL, 0);
            return;
        }
    static const struct { const char *n; unsigned fn; } shv[] = {{"sllv", 4}, {"srlv", 6}, {"srav", 7}};
    for (size_t i = 0; i < sizeof shv / sizeof *shv; i++)
        if (!strcmp(op, shv[i].n)) {
            put(R(reg_arg(st, 2), reg_arg(st, 1), reg_arg(st, 0), 0, shv[i].fn), 0, NULL, 0);
            return;
        }
    static const struct { const char *n; unsigned op; } imm[] = {
        {"addi", 8}, {"addiu", 9}, {"slti", 10}, {"sltiu", 11}, {"andi", 12}, {"ori", 13}, {"xori", 14}};
    for (size_t i = 0; i < sizeof imm / sizeof *imm; i++)
        if (!strcmp(op, imm[i].n)) {
            put(I(imm[i].op, reg_arg(st, 1), reg_arg(st, 0), (uint32_t)imm_arg(st, 2)), 0, NULL, 0);
            return;
        }
    static const struct { const char *n; unsigned op; } mem[] = {
        {"lb", 32}, {"lh", 33}, {"lwl", 34}, {"lw", 35}, {"lbu", 36}, {"lhu", 37}, {"lwr", 38},
        {"sb", 40}, {"sh", 41}, {"swl", 42}, {"sw", 43}, {"swr", 46}};
    for (size_t i = 0; i < sizeof mem / sizeof *mem; i++)
        if (!strcmp(op, mem[i].n)) {
            Mem m;
            if (n < 2) die("Unexpected end of line");
            parse_mem(st->args[1], &m);
            mem_op(mem[i].op, reg_arg(st, 0), &m);
            return;
        }

    if (!strcmp(op, "lui")) {
        Val v;
        if (n >= 2 && parse_val(st->args[1], &v) && v.sym && v.op == OP_SHR) {
            add_reloc_v(26, &v);
            emit_word(I(15, 0, reg_arg(st, 0), 0));
            return;
        }
        put(I(15, 0, reg_arg(st, 0), (uint32_t)imm_arg(st, 1)), 0, NULL, 0);
    } else if (!strcmp(op, "nop")) {
        put(0, 0, NULL, 0);
    } else if (!strcmp(op, "move")) {
        put(R(reg_arg(st, 1), 0, reg_arg(st, 0), 0, 33), 0, NULL, 0);
    } else if (!strcmp(op, "li")) {
        long v = imm_arg(st, 1);
        int rt = reg_arg(st, 0);
        if (v >= -32768 && v <= 32767) put(I(9, 0, rt, (uint32_t)v), 0, NULL, 0);
        else if (v >= 0 && v <= 65535) put(I(13, 0, rt, (uint32_t)v), 0, NULL, 0);
        else {
            put(I(15, 0, rt, (uint32_t)v >> 16), 0, NULL, 0);
            if ((uint32_t)v & 0xffff) put(I(13, rt, rt, (uint32_t)v), 0, NULL, 0);
        }
    } else if (!strcmp(op, "la")) {
        Mem m;
        int rt = reg_arg(st, 0);
        if (n < 2) die("Unexpected end of line");
        parse_mem(st->args[1], &m);
        if (!m.has_sym) { put(I(9, 0, rt, (uint32_t)m.v.value), 0, NULL, 0); return; }
        put(I(15, 0, rt, 0), REL_HI16, m.v.sym, m.v.value);
        put(I(9, rt, rt, 0), REL_LO16, m.v.sym, m.v.value);
    } else if (!strcmp(op, "j") || !strcmp(op, "jal")) {
        Val v;
        if (n < 1) die("Unexpected end of line");
        int r = parse_reg(st->args[0]);
        if (r >= 0) { put(op[1] == 'a' ? R(r, 0, 31, 0, 9) : R(r, 0, 0, 0, 8), 0, NULL, 0); return; }
        if (!parse_val(st->args[0], &v) || !v.sym) die("Syntax error in expression");
        put((op[1] == 'a' ? 3u : 2u) << 26, REL_JUMP, v.sym, v.value);
    } else if (!strcmp(op, "jr")) {
        put(R(reg_arg(st, 0), 0, 0, 0, 8), 0, NULL, 0);
    } else if (!strcmp(op, "jalr")) {
        if (n == 1) put(R(reg_arg(st, 0), 0, 31, 0, 9), 0, NULL, 0);
        else put(R(reg_arg(st, 1), 0, reg_arg(st, 0), 0, 9), 0, NULL, 0);
    } else if (!strcmp(op, "beq") || !strcmp(op, "bne")) {
        if (n < 3) die("Unexpected end of line");
        branch(I(op[1] == 'e' ? 4 : 5, reg_arg(st, 0), reg_arg(st, 1), 0), st->args[2]);
    } else if (!strcmp(op, "beqz") || !strcmp(op, "bnez")) {
        if (n < 2) die("Unexpected end of line");
        branch(I(op[1] == 'e' ? 4 : 5, reg_arg(st, 0), 0, 0), st->args[1]);
    } else if (!strcmp(op, "blez") || !strcmp(op, "bgtz")) {
        if (n < 2) die("Unexpected end of line");
        branch(I(op[1] == 'l' ? 6 : 7, reg_arg(st, 0), 0, 0), st->args[1]);
    } else if (!strcmp(op, "bltz") || !strcmp(op, "bgez")) {
        if (n < 2) die("Unexpected end of line");
        branch(I(1, reg_arg(st, 0), op[1] == 'l' ? 0 : 1, 0), st->args[1]);
    } else if (!strcmp(op, "mult") || !strcmp(op, "multu") || !strcmp(op, "div") || !strcmp(op, "divu")) {
        unsigned fn = !strcmp(op, "mult") ? 24 : !strcmp(op, "multu") ? 25 : !strcmp(op, "div") ? 26 : 27;
        put(R(reg_arg(st, 0), reg_arg(st, 1), 0, 0, fn), 0, NULL, 0);
    } else if (!strcmp(op, "mfhi") || !strcmp(op, "mflo")) {
        put(R(0, 0, reg_arg(st, 0), 0, op[2] == 'h' ? 16 : 18), 0, NULL, 0);
    } else if (!strcmp(op, "mthi") || !strcmp(op, "mtlo")) {
        put(R(reg_arg(st, 0), 0, 0, 0, op[2] == 'h' ? 17 : 19), 0, NULL, 0);
    } else {
        die("Op-code not recognised");
    }
}

/* ------------------------------------------------------------- directives */

static void data_directive(Stmt *st, int width)
{
    for (int i = 0; i < st->nargs; i++) {
        const char *a = st->args[i];
        if (a[0] == '"') {
            /* string: bytes */
            for (const char *p = a + 1; *p && *p != '"'; p++) {
                uint8_t c = (uint8_t)*p;
                emit_bytes(&c, 1);
            }
            continue;
        }
        Val v;
        if (!parse_val(a, &v)) die("Error in expression");
        uint32_t x = (uint32_t)v.value;
        if (v.sym) {
            if (width != 4) die("Error in expression");
            add_reloc(REL_WORD32, v.sym, v.value);
            x = 0;
        }
        uint8_t c[4] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24)};
        emit_bytes(c, (size_t)width);
    }
}

static void run_pass(int pass)
{
    g_pass = pass;
    g_cur = -1;
    g_scope = 0;
    for (int i = 0; i < g_nsect; i++) g_sect[i].loc = 0;
    for (size_t i = 0; i < g_nstmts; i++) {
        Stmt *st = &g_stmts[i];
        g_lineno = st->line;
        if (st->label) {
            if (st->label[0] != '@') g_scope++;
            Sym *s = sym_get(st->label);
            if (g_cur < 0) die("Section not specified");
            if (pass == 1) {
                if (s->defined) die("Label '%s' multiply defined", st->label);
                s->defined = 1;
                s->sect = g_cur;
                s->off = g_sect[g_cur].loc;
            } else {
                s->def_seen = 1;
            }
        }
        if (!st->op) continue;
        const char *op = st->op;
        if (!strcmp(op, "opt") || !strcmp(op, "disable") || !strcmp(op, "enable")) continue;
        if (!strcmp(op, "section")) {
            if (st->nargs < 1) die("Illegal section name");
            int idx = sect_find(st->args[0]);
            if (pass == 1 && idx < 0) {
                if (g_nsect == MAXSECT) die("Too many different sections");
                idx = g_nsect++;
                g_sect[idx].name = xstrdup(st->args[0]);
                g_sect[idx].id = idx + 2;
            }
            if (pass == 2) do_switch(idx);
            else g_cur = idx;
            continue;
        }
        if (!strcmp(op, "xdef")) {
            for (int k = 0; k < st->nargs; k++) {
                Sym *s = sym_get(st->args[k]);
                s->is_xdef = 1;
                if (pass == 1) sym_number(s);
            }
            continue;
        }
        if (!strcmp(op, "xref")) {
            for (int k = 0; k < st->nargs; k++) {
                Sym *s = sym_get(st->args[k]);
                s->is_xref = 1;
                if (pass == 1) sym_number(s);
            }
            continue;
        }
        if (!strcmp(op, "dw")) { data_directive(st, 4); continue; }
        if (!strcmp(op, "dh")) { data_directive(st, 2); continue; }
        if (!strcmp(op, "db")) { data_directive(st, 1); continue; }
        insn(st);
    }
}

static void write_symbols(Buf *o)
{
    for (int h = 0; h < HASH_SIZE; h++) {
        for (Sym *s = g_hash[h]; s; s = s->next) {
            if (!s->id) continue;
            if (s->is_xdef && s->defined) {
                b8(o, 12);
                b16(o, (unsigned)s->id);
                b16(o, (unsigned)g_sect[s->sect].id);
                b32(o, s->off);
                bstr(o, s->name);
            } else if (!s->defined) {
                b8(o, 14);
                b16(o, (unsigned)s->id);
                bstr(o, s->name);
            }
        }
    }
}

int main(int argc, char **argv)
{
    char *spec = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && argv[i][1] && !strchr(argv[i] + 1, '.') && !strchr(argv[i] + 1, ',')) {
            if (!strcmp(argv[i], "/l") || !strcmp(argv[i], "/q") || !strcmp(argv[i], "/c")) continue;
            fprintf(stderr, "Unknown option %s\n", argv[i]);
            return 1;
        }
        if (spec) { fprintf(stderr, "Too many file names specified\n"); return 1; }
        spec = argv[i];
    }
    if (!spec) { fprintf(stderr, "No source file specified\n"); return 1; }
    char *comma = strchr(spec, ',');
    if (!comma) { fprintf(stderr, "No source file specified\n"); return 1; }
    *comma = 0;
    const char *in = spec, *out = comma + 1;
    g_infile = in;

    FILE *f = fopen(in, "rb");
    if (!f) { fprintf(stderr, "Could not open file '%s'\n", in); return 1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int lineno = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        parse_line(line, lineno);
    }
    fclose(f);

    /* pass 1 assigns section ids, symbol numbers and label offsets; pass 2 emits */
    run_pass(1);
    g_nextid = g_nsect + 2;
    /* symbols declared with xdef/xref are numbered in declaration order */
    for (size_t i = 0; i < g_nstmts; i++) {
        Stmt *st = &g_stmts[i];
        if (st->op && (!strcmp(st->op, "xdef") || !strcmp(st->op, "xref")))
            for (int k = 0; k < st->nargs; k++) { Sym *s = sym_get(st->args[k]); s->id = 0; }
    }
    for (size_t i = 0; i < g_nstmts; i++) {
        Stmt *st = &g_stmts[i];
        if (st->op && (!strcmp(st->op, "xdef") || !strcmp(st->op, "xref")))
            for (int k = 0; k < st->nargs; k++) sym_number(sym_get(st->args[k]));
    }
    run_pass(2);
    flush_chunk();

    Buf o = {0};
    bput(&o, "LNK\x02", 4);
    b8(&o, 46); b8(&o, 7);
    for (int i = 0; i < g_nsect; i++) {
        b8(&o, 16); b16(&o, (unsigned)g_sect[i].id); b16(&o, 0); b8(&o, 8); bstr(&o, g_sect[i].name);
    }
    bput(&o, g_out.p, g_out.n);
    write_symbols(&o);
    b8(&o, 0);

    FILE *fo = fopen(out, "wb");
    if (!fo) { fprintf(stderr, "Error creating output file\n"); return 1; }
    fwrite(o.p, 1, o.n, fo);
    fclose(fo);
    return 0;
}
