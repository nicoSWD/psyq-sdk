/* Instruction encoding for aspsx. Included from aspsx.c (shares its statics). */

static uint32_t g_prev_load_reg; /* register written by the previous load (delay), 0 = none */
static int g_prev_branch;        /* previous emitted insn was a branch/jump (reorder fills delay slot) */
static Sym *g_branch_target;     /* set by branch_to: put_word patches the offset after pre-nops are placed */
static int g_gte_gap = 3;        /* instructions since the last lwc2/mtc2/ctc2 (capped) */
static int g_hilo_gap = 3;       /* instructions emitted since the last mfhi/mflo (capped) */
static unsigned g_numlab[10];    /* numeric local label counters: "1:" ... */

static void insn_reset(void)
{
    g_prev_load_reg = 0;
    g_prev_branch = 0;
    g_hilo_gap = 3;
    g_gte_gap = 3;
    memset(g_numlab, 0, sizeof g_numlab);
}

static int is_gte_feed(uint32_t w)   /* lwc2, mtc2, ctc2 */
{
    unsigned op = w >> 26, rs = (w >> 21) & 31;
    return op == 0x32 || (op == 0x12 && !(w & (1u << 25)) && (rs == 4 || rs == 6));
}
static int is_gte_cmd(uint32_t w) { return (w >> 26) == 0x12 && (w & (1u << 25)); }
static void note_gap(uint32_t w)
{
    if (is_gte_feed(w)) g_gte_gap = 0;
    else if (g_gte_gap < 3) g_gte_gap++;
}

/* registers read by an encoded instruction (bitmask) */
static uint32_t reads_mask(uint32_t w)
{
    unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63;
    uint32_t m = 0;
    switch (op) {
    case 0:
        switch (fn) {
        case 0: case 2: case 3: m = 1u << rt; break;                     /* sll srl sra */
        case 4: case 6: case 7: m = (1u << rs) | (1u << rt); break;      /* sllv srlv srav */
        case 8: case 9: m = 1u << rs; break;                             /* jr jalr */
        case 12: case 13: m = 0; break;                                  /* syscall break */
        case 16: case 18: m = 0; break;                                  /* mfhi mflo */
        case 17: case 19: m = 1u << rs; break;                           /* mthi mtlo */
        default: m = (1u << rs) | (1u << rt); break;
        }
        break;
    case 1: m = 1u << rs; break;
    case 2: case 3: m = 0; break;
    case 4: case 5: m = (1u << rs) | (1u << rt); break;
    case 6: case 7: m = 1u << rs; break;
    case 15: m = 0; break;                                               /* lui */
    case 0x10: case 0x12:
        if (op == 0x12 && (w & (1u << 25))) m = 0;                       /* cop2 command */
        else if (rs == 4 || rs == 6) m = 1u << rt;                       /* mtcz ctcz */
        break;
    case 0x22: case 0x26: m = 1u << rs; break;                           /* lwl lwr: rt merge is forwarded */
    case 0x2a: case 0x2e:                                                /* swl swr */
        m = (1u << rs) | (1u << rt); break;
    case 0x28: case 0x29: case 0x2b:                                     /* sb sh sw */
        m = (1u << rs) | (1u << rt); break;
    case 0x32: case 0x3a: m = 1u << rs; break;                           /* lwc2 swc2 */
    default:
        if (op >= 8 && op <= 14) m = 1u << rs;                           /* I-type alu */
        else if (op >= 0x20 && op <= 0x25) m = 1u << rs;                 /* loads */
        break;
    }
    return m & ~1u;
}

/* register written with a one-instruction delay (loads, mfc) or 0 */
static uint32_t load_dest(uint32_t w)
{
    unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    if (op >= 0x20 && op <= 0x26 && op != 0x27) return rt;
    if ((op == 0x10 || op == 0x12) && !(w & (1u << 25)) && (rs == 0 || rs == 2)) return rt;
    return 0;
}

static int is_branch_word(uint32_t w)
{
    unsigned op = w >> 26, fn = w & 63;
    if (op == 0) return fn == 8 || fn == 9;
    return (op >= 1 && op <= 7);
}

/* mnemonic of the next instruction statement after the current one (skips directives/labels) */
static const char *next_insn_op(void)
{
    for (size_t k = g_idx + 1; k < g_nstmts; k++) {
        Stmt *n = &g_stmts[k];
        if (n->label) return NULL;
        if (!n->op || n->op[0] == '.') continue;
        return n->op;
    }
    return NULL;
}

/* emit one machine word (pass 1 only counts) */
static void put_word(uint32_t w, int reloc_type, Sym *rs_, long addend)
{
    /* GTE: a cop2 command needs two instructions after lwc2/mtc2/ctc2 */
    if (is_gte_cmd(w)) {
        while (g_gte_gap < 2) {
            if (g_pass == 1) g_loc[g_cursect] += 4;
            else emit_word(0);
            g_gte_gap++;
            g_prev_load_reg = 0;
            if (g_hilo_gap < 3) g_hilo_gap++;
        }
    }
    /* HI/LO hazard: mult/div/mthi/mtlo within two instructions of mfhi/mflo */
    if ((w >> 26) == 0 && ((w & 63) == 17 || (w & 63) == 19 || ((w & 63) >= 24 && (w & 63) <= 27))) {
        while (g_hilo_gap < 2) {
            if (g_pass == 1) g_loc[g_cursect] += 4;
            else emit_word(0);
            g_hilo_gap++;
            if (g_gte_gap < 3) g_gte_gap++;
            g_prev_load_reg = 0;
        }
    }
    /* noreorder branch whose delay slot holds mult/div/mthi/mtlo: the HI/LO gap is resolved before the branch */
    if (is_branch_word(w) && !g_reorder) {
        const char *nx = next_insn_op();
        if (nx && (!strcmp(nx, "mult") || !strcmp(nx, "multu") || !strcmp(nx, "div") || !strcmp(nx, "divu") ||
                   !strcmp(nx, "mthi") || !strcmp(nx, "mtlo"))) {
            while (g_hilo_gap < 1) {
                if (g_pass == 1) g_loc[g_cursect] += 4;
                else emit_word(0);
                g_hilo_gap++;
                g_prev_load_reg = 0;
            }
        }
    }
    /* hazard: previous load result used right away */
    if (g_prev_load_reg && (reads_mask(w) & (1u << g_prev_load_reg))) {
        if (g_pass == 1) g_loc[g_cursect] += 4;
        else emit_word(0);
        if (g_hilo_gap < 3) g_hilo_gap++;
        if (g_gte_gap < 3) g_gte_gap++;
    }
    if (g_branch_target && g_pass == 2) {
        Sym *bs = g_branch_target;
        if (!bs->defined || bs->sect != g_cursect) die("Branch to undefined or foreign label '%s'", bs->name);
        long off = ((long)bs->off - ((long)g_loc[g_cursect] + 4)) >> 2;
        w = (w & 0xffff0000u) | ((uint32_t)off & 0xffff);
    }
    g_branch_target = NULL;
    if (g_pass == 1) {
        g_loc[g_cursect] += 4;
    } else {
        if (getenv("ASPSX_TRACE") && g_cursect == S_TEXT) fprintf(stderr, "T %d %x\n", g_lineno, g_loc[g_cursect]);
        if (reloc_type) add_reloc(reloc_type, (unsigned)g_chunk.n, rs_, addend);
        emit_word(w);
    }
    /* the hazard nop is decided by the producer's mode; cfc2 always needs it */
    g_prev_load_reg = (g_reorder || ((w >> 26) == 0x12 && ((w >> 21) & 31) == 2)) ? load_dest(w) : 0;
    if ((w >> 26) == 0 && ((w & 63) == 16 || (w & 63) == 18)) g_hilo_gap = 0;
    else if (g_hilo_gap < 3) g_hilo_gap++;
    note_gap(w);
    if (is_branch_word(w) && g_reorder) {
        /* reorder mode: assembler supplies the delay slot */
        if (g_pass == 1) g_loc[g_cursect] += 4;
        else emit_word(0);
        g_prev_load_reg = 0;
        if (g_hilo_gap < 3) g_hilo_gap++;
        if (g_gte_gap < 3) g_gte_gap++;
    }
}

static void put_raw(uint32_t w)
{
    if (g_pass == 1) g_loc[g_cursect] += 4;
    else emit_word(w);
    g_prev_load_reg = 0;
    if ((w >> 26) == 0 && ((w & 63) == 16 || (w & 63) == 18)) g_hilo_gap = 0;
    else if (g_hilo_gap < 3) g_hilo_gap++;
    note_gap(w);
}

#define R(w, rs, rt, rd, sh, fn) \
    ((uint32_t)(((rs) & 31) << 21 | ((rt) & 31) << 16 | ((rd) & 31) << 11 | ((sh) & 31) << 6 | (fn)))
#define I(op, rs, rt, imm) ((uint32_t)((op) << 26 | ((rs) & 31) << 21 | ((rt) & 31) << 16 | ((imm) & 0xffff)))

static int reg_arg(Stmt *st, int i)
{
    if (i >= st->nargs) die("Unexpected end of line");
    int r = parse_reg(st->args[i]);
    if (r < 0) die("Illegal register");
    return r;
}

static long imm_arg(Stmt *st, int i)
{
    if (i >= st->nargs) die("Unexpected end of line");
    Val v;
    if (!parse_val(st->args[i], &v, 0) || v.sym) die("Syntax error in expression");
    return v.value;
}

/* resolve numeric local label references (1f / 1b) to a unique symbol name */
static const char *local_name(const char *s, char *buf, size_t n)
{
    if (isdigit((unsigned char)s[0]) && !s[1]) { /* definition "1" handled elsewhere */
        return s;
    }
    if (isdigit((unsigned char)s[0]) && (s[1] == 'f' || s[1] == 'b') && !s[2]) {
        unsigned d = (unsigned)(s[0] - '0');
        unsigned idx = g_numlab[d] + (s[1] == 'f' ? 1 : 0);
        snprintf(buf, n, "%c#%u", s[0], idx);
        return buf;
    }
    return s;
}

/* ---- memory operand: off(base) | (base) | sym[+off] | sym[+off](base) */
typedef struct {
    int base;      /* -1 = none */
    Val v;
    int has_sym;
    int rel;       /* explicit %hi/%lo/%gp_rel operator: reloc type, 0 = none */
} Mem;

/* "%hi(sym+4)" -> REL_HI16 + value; returns 0 if not a %op(...) operand */
static int parse_pct(const char *a, int *rel, Val *v)
{
    char nm[256];
    a = trim((char *)a);
    if (*a != '%') return 0;
    const char *lp = strchr(a, '(');
    size_t l = strlen(a);
    if (!lp || a[l - 1] != ')') die("Syntax error in expression");
    if (!strncmp(a, "%hi(", 4)) *rel = REL_HI16;
    else if (!strncmp(a, "%lo(", 4)) *rel = REL_LO16;
    else if (!strncmp(a, "%gp_rel(", 8)) *rel = REL_GPREL;
    else die("Syntax error in expression");
    char inner[256];
    snprintf(inner, sizeof inner, "%.*s", (int)(l - 1 - (size_t)(lp + 1 - a)), lp + 1);
    const char *t = local_name(trim(inner), nm, sizeof nm);
    if (!parse_val(t, v, 1)) die("Syntax error in expression");
    return 1;
}

static void parse_mem(const char *a, Mem *m)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", a);
    m->base = -1;
    char *lp = strrchr(tmp, '(');
    if (lp && tmp[strlen(tmp) - 1] == ')') {
        tmp[strlen(tmp) - 1] = 0;
        *lp = 0;
        m->base = parse_reg(trim(lp + 1));
        if (m->base < 0) die("Illegal register");
    }
    char nm[256];
    const char *t = trim(tmp);
    m->rel = 0;
    if (*t == '%') {
        parse_pct(t, &m->rel, &m->v);
        m->has_sym = m->v.sym != NULL;
        return;
    }
    if (!*t) { m->v.sym = NULL; m->v.value = 0; m->has_sym = 0; return; }
    t = local_name(t, nm, sizeof nm);
    if (!parse_val(t, &m->v, 1)) die("Syntax error in expression");
    m->has_sym = m->v.sym != NULL;
}

static void mem_op(unsigned opc, int rt, Mem *m)
{
    if (!m->has_sym) {
        int base = m->base < 0 ? 0 : m->base;
        long val = m->v.value;
        if (val >= -32768 && val <= 32767) {
            put_word(I(opc, base, rt, (uint32_t)val), 0, NULL, 0);
            return;
        }
        /* absolute address outside 16 bits: lui tmp,hi ; [addu tmp,tmp,base] ; op rt,lo(tmp) */
        int tmp = (opc == 32 || opc == 33 || opc == 35 || opc == 36 || opc == 37) ? rt : 1;
        uint32_t u = (uint32_t)val;
        put_word(I(15, 0, tmp, ((u + 0x8000u) >> 16) & 0xffff), 0, NULL, 0);
        if (base) put_word(R(0, tmp, base, tmp, 0, 33), 0, NULL, 0);
        put_word(I(opc, tmp, rt, u & 0xffff), 0, NULL, 0);
        return;
    }
    Sym *s = m->v.sym;
    if (m->rel) {
        int base = m->base < 0 ? 0 : m->base;
        put_word(I(opc, base, rt, 0), m->rel, s, m->v.value);
        return;
    }
    if (m->base >= 0 && !m->rel) {
        /* sym(base): lui $at,%hi ; addu $at,$at,base ; op rt,%lo($at) */
        put_word(I(15, 0, 1, 0), REL_HI16, s, m->v.value);
        put_word(R(0, 1, m->base, 1, 0, 33), 0, NULL, 0);
        put_word(I(opc, 1, rt, 0), REL_LO16, s, m->v.value);
        return;
    }
    if (m->base < 0 && is_small_sym(s)) {
        put_word(I(opc, 28, rt, 0), REL_GPREL, s, m->v.value);
        return;
    }
    /* lui tmp,%hi ; op rt,%lo(tmp): a plain GPR load reuses its destination as tmp, everything else $at */
    int tmp = (opc == 32 || opc == 33 || opc == 35 || opc == 36 || opc == 37) ? rt : 1;
    put_word(I(15, 0, tmp, 0), REL_HI16, s, m->v.value);
    put_word(I(opc, tmp, rt, 0), REL_LO16, s, m->v.value);
}

static void branch_to(uint32_t base_word, const char *target)
{
    char nm[256];
    const char *t = local_name(target, nm, sizeof nm);
    g_branch_target = sym_get(t);
    put_word(base_word, 0, NULL, 0);
}

/* 3-operand div/divu/rem/remu: divide, zero check, (signed only) overflow check, move result */
static void div_macro(Stmt *st, unsigned fn, int want_rem)
{
    if (st->nargs != 3) die("Illegal addressing mode");
    int rd = reg_arg(st, 0), rs = reg_arg(st, 1), rt = parse_reg(st->args[2]);
    if (rt < 0) {
        /* immediate divisor goes through $at */
        long v = imm_arg(st, 2);
        put_word(I(9, 0, 1, (uint32_t)v), 0, NULL, 0);
        rt = 1;
    }
    put_word(R(0, rs, rt, 0, 0, fn), 0, NULL, 0);
    put_raw(I(5, rt, 0, 2));            /* bne rt,$0,+2 */
    put_raw(0);                         /* nop (delay slot) */
    put_raw((7u << 16) | 13); /* break 7 */
    if (fn == 26) {
        put_raw(I(9, 0, 1, 0xffff));    /* addiu $at,$0,-1 */
        put_raw(I(5, rt, 1, 4));        /* bne rt,$at,+4 */
        put_raw(I(15, 0, 1, 0x8000));   /* lui $at,0x8000 (delay slot) */
        put_raw(I(5, rs, 1, 2));        /* bne rs,$at,+2 */
        put_raw(0);                     /* nop */
        put_raw((6u << 16) | 13); /* break 6 */
    }
    put_raw(R(0, 0, 0, rd, 0, want_rem ? 16 : 18)); /* mfhi / mflo */
    if (g_reorder) g_prev_load_reg = (uint32_t)rd;   /* macro result is treated like a load result */
}

static void li_imm(int rt, long v)
{
    if (v >= -32768 && v <= 32767) put_word(I(9, 0, rt, (uint32_t)v), 0, NULL, 0);
    else if (v >= 0 && v <= 65535) put_word(I(13, 0, rt, (uint32_t)v), 0, NULL, 0);
    else {
        put_word(I(15, 0, rt, (uint32_t)((uint32_t)v >> 16)), 0, NULL, 0);
        if ((uint32_t)v & 0xffff) put_word(I(13, rt, rt, (uint32_t)v), 0, NULL, 0);
    }
}

static void insn(Stmt *st)
{
    const char *op = st->op;
    int n = st->nargs;
    char nm[256];

    /* three register ALU */
    static const struct { const char *n; unsigned fn; } alu3[] = {
        {"add", 32}, {"addu", 33}, {"sub", 34}, {"subu", 35}, {"and", 36}, {"or", 37},
        {"xor", 38}, {"nor", 39}, {"slt", 42}, {"sltu", 43}};
    for (size_t i = 0; i < sizeof alu3 / sizeof *alu3; i++)
        if (!strcmp(op, alu3[i].n)) {
            if (n == 3 && parse_reg(st->args[2]) < 0) {
                /* register, register, immediate: immediate form of the same op */
                long v = imm_arg(st, 2);
                unsigned iop = 0;
                if (!strcmp(op, "add") || !strcmp(op, "addu")) iop = 9;
                else if (!strcmp(op, "sub") || !strcmp(op, "subu")) { iop = 9; v = -v; }
                else if (!strcmp(op, "and")) iop = 12;
                else if (!strcmp(op, "or")) iop = 13;
                else if (!strcmp(op, "xor")) iop = 14;
                else if (!strcmp(op, "slt")) iop = 10;
                else if (!strcmp(op, "sltu")) {
                    if (v < 0) {
                        /* negative immediate: go through $at, sltiu would sign-extend */
                        put_word(I(9, 0, 1, (uint32_t)v), 0, NULL, 0);
                        put_word(R(0, reg_arg(st, 1), 1, reg_arg(st, 0), 0, 43), 0, NULL, 0);
                        return;
                    }
                    iop = 11;
                }
                else die("Illegal register");
                put_word(I(iop, reg_arg(st, 1), reg_arg(st, 0), (uint32_t)v), 0, NULL, 0);
                return;
            }
            put_word(R(0, reg_arg(st, 1), reg_arg(st, 2), reg_arg(st, 0), 0, alu3[i].fn), 0, NULL, 0);
            return;
        }
    static const struct { const char *n; unsigned fn; } sh[] = {{"sll", 0}, {"srl", 2}, {"sra", 3}};
    for (size_t i = 0; i < sizeof sh / sizeof *sh; i++)
        if (!strcmp(op, sh[i].n)) {
            if (n == 3 && parse_reg(st->args[2]) >= 0) {
                /* variable shift: sllv/srlv/srav rd,rt,rs */
                put_word(R(0, reg_arg(st, 2), reg_arg(st, 1), reg_arg(st, 0), 0, sh[i].fn + 4), 0, NULL, 0);
                return;
            }
            put_word(R(0, 0, reg_arg(st, 1), reg_arg(st, 0), (unsigned)imm_arg(st, 2), sh[i].fn), 0, NULL, 0);
            return;
        }
    static const struct { const char *n; unsigned op; } imm[] = {
        {"addi", 8}, {"addiu", 9}, {"slti", 10}, {"sltiu", 11}, {"andi", 12}, {"ori", 13}, {"xori", 14}};
    for (size_t i = 0; i < sizeof imm / sizeof *imm; i++)
        if (!strcmp(op, imm[i].n)) {
            int rel;
            Val pv;
            if (n >= 3 && parse_pct(st->args[2], &rel, &pv)) {
                put_word(I(imm[i].op, reg_arg(st, 1), reg_arg(st, 0), 0), rel, pv.sym, pv.value);
                return;
            }
            put_word(I(imm[i].op, reg_arg(st, 1), reg_arg(st, 0), (uint32_t)imm_arg(st, 2)), 0, NULL, 0);
            return;
        }
    static const struct { const char *n; unsigned op; } mem[] = {
        {"lb", 32}, {"lh", 33}, {"lwl", 34}, {"lw", 35}, {"lbu", 36}, {"lhu", 37}, {"lwr", 38},
        {"sb", 40}, {"sh", 41}, {"swl", 42}, {"sw", 43}, {"swr", 46}, {"lwc2", 50}, {"swc2", 58}};
    for (size_t i = 0; i < sizeof mem / sizeof *mem; i++)
        if (!strcmp(op, mem[i].n)) {
            Mem m;
            if (n < 2) die("Unexpected end of line");
            int rt;
            if (mem[i].op == 50 || mem[i].op == 58) {
                /* GTE data register written as $n or number */
                int r = parse_reg(st->args[0]);
                rt = r >= 0 ? r : (int)imm_arg(st, 0);
            } else {
                rt = reg_arg(st, 0);
            }
            parse_mem(st->args[1], &m);
            mem_op(mem[i].op, rt, &m);
            return;
        }

    if (!strcmp(op, "lui")) {
        Val v;
        int rel;
        if (n >= 2 && parse_pct(st->args[1], &rel, &v)) {
            put_word(I(15, 0, reg_arg(st, 0), 0), rel, v.sym, v.value);
            return;
        }
        if (n < 2 || !parse_val(st->args[1], &v, 0) || v.sym) die("Syntax error in expression");
        put_word(I(15, 0, reg_arg(st, 0), (uint32_t)v.value), 0, NULL, 0);
    } else if (!strcmp(op, "nop")) {
        put_word(0, 0, NULL, 0);
    } else if (!strcmp(op, "move")) {
        put_word(R(0, reg_arg(st, 1), 0, reg_arg(st, 0), 0, 33), 0, NULL, 0);
    } else if (!strcmp(op, "li")) {
        li_imm(reg_arg(st, 0), imm_arg(st, 1));
    } else if (!strcmp(op, "la")) {
        Mem m;
        int rt = reg_arg(st, 0);
        if (n < 2) die("Unexpected end of line");
        parse_mem(st->args[1], &m);
        if (!m.has_sym) { li_imm(rt, m.v.value); return; }
        put_word(I(15, 0, rt, 0), REL_HI16, m.v.sym, m.v.value);
        put_word(I(9, rt, rt, 0), REL_LO16, m.v.sym, m.v.value);
    } else if (!strcmp(op, "j") || !strcmp(op, "jal")) {
        int link = op[1] == 'a';
        if (n < 1) die("Unexpected end of line");
        int r = parse_reg(st->args[0]);
        if (r >= 0 && n == 2) {
            put_word(R(0, reg_arg(st, 1), 0, r, 0, 9), 0, NULL, 0);   /* jal rd,rs == jalr rd,rs */
        } else if (r >= 0) {
            put_word(link ? R(0, r, 0, 31, 0, 9) : R(0, r, 0, 0, 0, 8), 0, NULL, 0);
        } else {
            Val v;
            if (!parse_val(local_name(st->args[0], nm, sizeof nm), &v, 1) || !v.sym) die("Syntax error in expression");
            put_word((link ? 3u : 2u) << 26, REL_JUMP, v.sym, v.value);
        }
    } else if (!strcmp(op, "jr")) {
        put_word(R(0, reg_arg(st, 0), 0, 0, 0, 8), 0, NULL, 0);
    } else if (!strcmp(op, "jalr")) {
        if (n == 1) put_word(R(0, reg_arg(st, 0), 0, 31, 0, 9), 0, NULL, 0);
        else put_word(R(0, reg_arg(st, 1), 0, reg_arg(st, 0), 0, 9), 0, NULL, 0);
    } else if (!strcmp(op, "beq") || !strcmp(op, "bne")) {
        if (n < 3) die("Unexpected end of line");
        branch_to(I(op[1] == 'e' ? 4 : 5, reg_arg(st, 0), reg_arg(st, 1), 0), st->args[2]);
    } else if (!strcmp(op, "blez") || !strcmp(op, "bgtz")) {
        if (n < 2) die("Unexpected end of line");
        branch_to(I(op[1] == 'l' ? 6 : 7, reg_arg(st, 0), 0, 0), st->args[1]);
    } else if (!strcmp(op, "bltz") || !strcmp(op, "bgez")) {
        if (n < 2) die("Unexpected end of line");
        branch_to(I(1, reg_arg(st, 0), op[1] == 'l' ? 0 : 1, 0), st->args[1]);
    } else if (!strcmp(op, "mult") || !strcmp(op, "multu") || !strcmp(op, "div") || !strcmp(op, "divu")) {
        static const struct { const char *n; unsigned fn; } md[] = {
            {"mult", 24}, {"multu", 25}, {"div", 26}, {"divu", 27}};
        unsigned fn = 0;
        for (size_t i = 0; i < 4; i++) if (!strcmp(op, md[i].n)) fn = md[i].fn;
        if (n == 2) put_word(R(0, reg_arg(st, 0), reg_arg(st, 1), 0, 0, fn), 0, NULL, 0);
        else div_macro(st, fn, 0);
    } else if (!strcmp(op, "rem") || !strcmp(op, "remu")) {
        div_macro(st, op[3] == 'u' ? 27 : 26, 1);
    } else if (!strcmp(op, "mfhi") || !strcmp(op, "mflo")) {
        put_word(R(0, 0, 0, reg_arg(st, 0), 0, op[2] == 'h' ? 16 : 18), 0, NULL, 0);
    } else if (!strcmp(op, "mthi") || !strcmp(op, "mtlo")) {
        put_word(R(0, reg_arg(st, 0), 0, 0, 0, op[2] == 'h' ? 17 : 19), 0, NULL, 0);
    } else if (!strcmp(op, "break")) {
        put_word((uint32_t)(imm_arg(st, 0) << 6) | 13, 0, NULL, 0);
    } else if (!strcmp(op, "mfc2") || !strcmp(op, "cfc2") || !strcmp(op, "mtc2") || !strcmp(op, "ctc2")) {
        unsigned rsf = !strcmp(op, "mfc2") ? 0 : !strcmp(op, "cfc2") ? 2 : !strcmp(op, "mtc2") ? 4 : 6;
        int rt = reg_arg(st, 0);
        int r = parse_reg(st->args[1]);
        int cr = r >= 0 ? r : (int)imm_arg(st, 1);
        put_word((0x12u << 26) | (rsf << 21) | ((unsigned)rt << 16) | ((unsigned)cr << 11), 0, NULL, 0);
    } else if (!strcmp(op, "cop2")) {
        put_word((0x12u << 26) | (1u << 25) | ((uint32_t)imm_arg(st, 0) & 0x1ffffff), 0, NULL, 0);
    } else {
        die("Op-code not recognised");
    }
}

/* numeric local labels: called from define_label when a bare digit label is seen */
static const char *numeric_label_name(const char *name, char *buf, size_t n)
{
    if (isdigit((unsigned char)name[0]) && !name[1]) {
        unsigned d = (unsigned)(name[0] - '0');
        g_numlab[d]++;
        snprintf(buf, n, "%c#%u", name[0], g_numlab[d]);
        return buf;
    }
    return name;
}
