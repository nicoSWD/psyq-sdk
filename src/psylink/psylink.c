/*
 * psylink - re-implementation of the SN Systems / PsyQ linker (psylink.exe, psylink_overlay_fopen_mod.exe).
 *
 * Clean-room: behaviour is derived from observing real input/output pairs.
 *
 * usage: psylink [/l LIBDIR] [/c] [/n N] [/q] [/gp SECTION] [/m] "@cmdfile",out.cpe[,out.sym[,out.map]]
 *
 * Reads SN "LNK" version 2 objects and "LIB" archives, lays the sections out as the command file asks,
 * resolves and applies relocations and writes a CPE image, "file()" group images and a link map.  When run
 * under a name containing "fopen_mod" (psylink_overlay_fopen_mod) group images are written on top of an
 * already existing file instead of being created.
 */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ util */

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...)
{
    va_list ap;
    fputs("psylink: Error : ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *r = xmalloc(n + 1);
    memcpy(r, s, n);
    return r;
}

static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

#define PUSH(arr, n, item) \
    do { (arr) = xrealloc((arr), sizeof(*(arr)) * ((size_t)(n) + 1)); (arr)[(n)++] = (item); } while (0)

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open '%s'", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = xmalloc((size_t)n + 1);
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) die("cannot read '%s'", path);
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void write_file(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot create '%s'", path);
    if (n && fwrite(d, 1, n, f) != n) die("cannot write '%s'", path);
    fclose(f);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* growable byte buffer */
typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void bput(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = xrealloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b8(Buf *b, unsigned v) { uint8_t c = (uint8_t)v; bput(b, &c, 1); }
static void b32(Buf *b, uint32_t v) { b8(b, v & 255); b8(b, (v >> 8) & 255); b8(b, (v >> 16) & 255); b8(b, v >> 24); }

/* ------------------------------------------------------- string sets/maps */

/* open addressing string map; a value of 0 doubles as "removed" for the sets built on top of it */
typedef struct {
    char **keys;
    uint32_t *vals;
    size_t cap, n;
} SMap;

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static void smap_init(SMap *m)
{
    m->cap = 1024;
    m->n = 0;
    m->keys = xmalloc(m->cap * sizeof *m->keys);
    m->vals = xmalloc(m->cap * sizeof *m->vals);
}

/* slot index when found (>= 0), or -(free slot) - 1 */
static long smap_slot(const SMap *m, const char *k)
{
    size_t i = hash_str(k) & (m->cap - 1);
    while (m->keys[i]) {
        if (!strcmp(m->keys[i], k)) return (long)i;
        i = (i + 1) & (m->cap - 1);
    }
    return -(long)i - 1;
}

static void smap_grow(SMap *m)
{
    SMap o = *m;
    m->cap *= 2;
    m->n = 0;
    m->keys = xmalloc(m->cap * sizeof *m->keys);
    m->vals = xmalloc(m->cap * sizeof *m->vals);
    for (size_t i = 0; i < o.cap; i++) {
        if (o.keys[i]) {
            long s = smap_slot(m, o.keys[i]);
            m->keys[-s - 1] = o.keys[i];
            m->vals[-s - 1] = o.vals[i];
            m->n++;
        }
    }
    free(o.keys);
    free(o.vals);
}

static void smap_put(SMap *m, const char *k, uint32_t v, int overwrite)
{
    long s = smap_slot(m, k);
    if (s >= 0) {
        if (overwrite) m->vals[s] = v;
        return;
    }
    if ((m->n + 1) * 2 > m->cap) {
        smap_grow(m);
        s = smap_slot(m, k);
    }
    m->keys[-s - 1] = xstrdup(k);
    m->vals[-s - 1] = v;
    m->n++;
}

static int smap_get(const SMap *m, const char *k, uint32_t *v)
{
    long s = smap_slot(m, k);
    if (s < 0) return 0;
    if (v) *v = m->vals[s];
    return 1;
}

static void set_add(SMap *m, const char *k) { smap_put(m, k, 1, 1); }
static int set_has(const SMap *m, const char *k)
{
    uint32_t v;
    return smap_get(m, k, &v) && v;
}
static void set_del(SMap *m, const char *k)
{
    long s = smap_slot(m, k);
    if (s >= 0) m->vals[s] = 0;
}

/* ---------------------------------------------------------- LNK objects */

enum { E_CONST = 1, E_SYM, E_SEC, E_X, E_OP };

typedef struct {
    int kind;
    uint32_t val;   /* constant, symbol/section id, or operator */
    int xtype;      /* E_X: the node type byte (12 = section start, 22 = section end, ...) */
    int a, b;       /* operand expressions (E_OP) */
} Expr;

typedef struct {
    uint16_t id;
    char *name;
    int has_base;
    uint32_t base;
} Sec;

typedef struct { uint16_t id; char *name; } XRef;
typedef struct { uint16_t id, sect; uint32_t off; char *name; } XDef;
typedef struct { uint16_t id, sect; uint32_t size; char *name; } Comm;

enum { I_SWITCH, I_CODE, I_ZEROES, I_RELOC };
typedef struct {
    int kind;
    uint32_t n;            /* section id / code length / zero count / reloc type */
    const uint8_t *data;   /* code bytes */
    uint32_t at;           /* reloc offset */
    int expr;              /* reloc expression */
} Item;

typedef struct {
    char *path;
    char *group;           /* group the object was included into, or NULL */
    Sec *sec; int nsec;
    Item *it; int nit;
    XRef *xr; int nxr;
    XDef *xd; int nxd;
    Comm *cm; int ncm;
    Expr *ex; int nex;
} Obj;

static void set_sec(Obj *o, uint16_t id, char *name)
{
    for (int i = 0; i < o->nsec; i++) {
        if (o->sec[i].id == id) { o->sec[i].name = name; return; }
    }
    Sec s = {id, name, 0, 0};
    PUSH(o->sec, o->nsec, s);
}

static const char *sec_name(const Obj *o, unsigned id)
{
    for (int i = 0; i < o->nsec; i++)
        if (o->sec[i].id == id) return o->sec[i].name;
    return NULL;
}

/* base address of the object's section called `name`, if it was laid out (the last such section wins) */
static int sec_base(const Obj *o, const char *name, uint32_t *base)
{
    int found = 0;
    for (int i = 0; i < o->nsec; i++) {
        if (o->sec[i].has_base && !strcmp(o->sec[i].name, name)) {
            *base = o->sec[i].base;
            found = 1;
        }
    }
    return found;
}

static int parse_expr(Obj *o, const uint8_t *d, size_t len, size_t *pp)
{
    size_t p = *pp;
    if (p >= len) die("%s: truncated expression", o->path);
    unsigned t = d[p++];
    Expr e = {0, 0, 0, -1, -1};
    if (t == 0) {
        if (p + 4 > len) die("%s: truncated expression", o->path);
        e.kind = E_CONST;
        e.val = rd32(d + p);
        p += 4;
    } else if (t == 2 || t == 4 || (t >= 6 && t <= 24 && !(t & 1))) {
        if (p + 2 > len) die("%s: truncated expression", o->path);
        e.kind = t == 2 ? E_SYM : t == 4 ? E_SEC : E_X;
        e.xtype = (int)t;
        e.val = rd16(d + p);
        p += 2;
    } else {
        e.kind = E_OP;
        e.val = t;
        *pp = p;
        e.a = parse_expr(o, d, len, pp);
        e.b = parse_expr(o, d, len, pp);
        p = *pp;
    }
    PUSH(o->ex, o->nex, e);
    *pp = p;
    return o->nex - 1;
}

static Obj *parse_obj(const char *path, const uint8_t *d, size_t len)
{
    Obj *o = xmalloc(sizeof *o);
    o->path = xstrdup(path);
    if (len < 4 || memcmp(d, "LNK", 3) || d[3] != 2) die("%s: not an LNK v2 object", path);
    size_t p = 4;
#define NEED(n) do { if (p + (size_t)(n) > len) die("%s: truncated record", path); } while (0)
    while (p < len) {
        unsigned c = d[p++];
        if (c == 0) break;
        switch (c) {
        case 2: {                           /* code */
            NEED(2);
            uint32_t n = rd16(d + p);
            p += 2;
            NEED(n);
            Item it = {I_CODE, n, d + p, 0, 0};
            PUSH(o->it, o->nit, it);
            p += n;
            break;
        }
        case 6: {                           /* switch section */
            NEED(2);
            Item it = {I_SWITCH, rd16(d + p), NULL, 0, 0};
            PUSH(o->it, o->nit, it);
            p += 2;
            break;
        }
        case 8: {                           /* zero fill */
            NEED(4);
            Item it = {I_ZEROES, rd32(d + p), NULL, 0, 0};
            PUSH(o->it, o->nit, it);
            p += 4;
            break;
        }
        case 10: {                          /* relocation */
            NEED(3);
            Item it = {I_RELOC, d[p], NULL, rd16(d + p + 1), 0};
            p += 3;
            it.expr = parse_expr(o, d, len, &p);
            PUSH(o->it, o->nit, it);
            break;
        }
        case 12: {                          /* xdef */
            NEED(9);
            XDef x = {rd16(d + p), rd16(d + p + 2), rd32(d + p + 4), NULL};
            p += 8;
            size_t n = d[p++];
            NEED(n);
            x.name = xstrndup((const char *)d + p, n);
            p += n;
            PUSH(o->xd, o->nxd, x);
            break;
        }
        case 14: {                          /* xref */
            NEED(3);
            XRef x = {rd16(d + p), NULL};
            p += 2;
            size_t n = d[p++];
            NEED(n);
            x.name = xstrndup((const char *)d + p, n);
            p += n;
            int i;
            for (i = 0; i < o->nxr; i++) {
                if (o->xr[i].id == x.id) { o->xr[i] = x; break; }
            }
            if (i == o->nxr) PUSH(o->xr, o->nxr, x);
            break;
        }
        case 16: {                          /* section: id, group, alignment, name */
            NEED(6);
            uint16_t id = rd16(d + p);
            p += 5;
            size_t n = d[p++];
            NEED(n);
            set_sec(o, id, xstrndup((const char *)d + p, n));
            p += n;
            break;
        }
        case 18:                            /* local symbol (not needed to link) */
            NEED(7);
            p += 6;
            { size_t n = d[p++]; NEED(n); p += n; }
            break;
        case 28:                            /* source file name */
            NEED(3);
            p += 2;
            { size_t n = d[p++]; NEED(n); p += n; }
            break;
        case 46:                            /* cpu type */
            NEED(1);
            p += 1;
            break;
        case 48: {                          /* common symbol */
            NEED(9);
            Comm m = {rd16(d + p), rd16(d + p + 2), rd32(d + p + 4), NULL};
            p += 8;
            size_t n = d[p++];
            NEED(n);
            m.name = xstrndup((const char *)d + p, n);
            p += n;
            PUSH(o->cm, o->ncm, m);
            break;
        }
        default:
            die("%s: unsupported record %u", path, c);
        }
    }
#undef NEED
    return o;
}

/* bytes the object contributes to one of its sections */
static uint32_t sect_size(const Obj *o, unsigned sid)
{
    uint32_t n = 0;
    long cur = -1;
    for (int i = 0; i < o->nit; i++) {
        const Item *it = &o->it[i];
        if (it->kind == I_SWITCH) cur = it->n;
        else if (cur == (long)sid && (it->kind == I_CODE || it->kind == I_ZEROES)) n += it->n;
    }
    return n;
}

/* ----------------------------------------------------------- LIB archive */

typedef struct {
    char *name;
    char **syms; int nsyms;
    const uint8_t *data; size_t len;
} Member;

typedef struct {
    char *path;
    Member *m; int nm;
} Lib;

static Lib *load_lib(const char *path)
{
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (len < 4 || memcmp(d, "LIB\x01", 4)) die("%s: not a LIB archive", path);
    Lib *l = xmalloc(sizeof *l);
    l->path = xstrdup(path);
    size_t p = 4;
    while (p + 20 <= len) {
        Member m = {0};
        size_t hdr = rd32(d + p + 12), total = rd32(d + p + 16);    /* header length, whole entry length */
        if (total == 0 || p + total > len || hdr > total) break;
        m.name = xstrndup((const char *)d + p, 8);
        for (int i = 7; i >= 0 && (m.name[i] == ' ' || m.name[i] == 0); i--) m.name[i] = 0;
        size_t q = p + 20;
        while (q < len && d[q] != 0) {
            size_t n = d[q];
            PUSH(m.syms, m.nsyms, xstrndup((const char *)d + q + 1, n));
            q += 1 + n;
        }
        m.data = d + p + hdr;
        m.len = total - hdr;
        PUSH(l->m, l->nm, m);
        p += total;
    }
    return l;
}

/* --------------------------------------------------------- command file */

typedef struct {
    char *name;
    char *kind;              /* "bss", ... or NULL */
    char *file;              /* file("...") output or NULL */
    char **secs; int nsecs;
} Group;

typedef struct { char *path; char *group; } Include;
typedef struct { char *name; int align; } SAlign;

typedef struct {
    int have_org;
    uint32_t org;
    Include *inc; int ninc;
    char **libs; char **lib_groups; int nlibs;
    Group *grp; int ngrp;
    char *pc_sym;
    SAlign *sal; int nsal;
} Cmd;

static int starts_with_ci(const char *s, const char *kw)
{
    while (*kw) {
        if (tolower((unsigned char)*s) != *kw) return 0;
        s++; kw++;
    }
    return 1;
}

static int is_word(int c) { return isalnum(c) || c == '_'; }

static const char *skip_ws(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* "path" or "path (unterminated) [, group] */
static void parse_path_and_group(const char *s, char **path, char **group)
{
    s = skip_ws(s);
    if (*s != '"') die("bad include line");
    s++;
    const char *e = s;
    while (*e && *e != '"') e++;
    *path = xstrndup(s, (size_t)(e - s));
    if (*e == '"') e++;
    e = skip_ws(e);
    *group = NULL;
    if (*e == ',') {
        e = skip_ws(e + 1);
        const char *w = e;
        while (is_word((unsigned char)*e)) e++;
        if (e > w) *group = xstrndup(w, (size_t)(e - w));
    }
}

static Cmd *parse_cmd(const char *text)
{
    Cmd *c = xmalloc(sizeof *c);
    char *copy = xstrdup(text);
    for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
        char *semi = strchr(line, ';');
        if (semi) *semi = 0;
        for (char *r = line; *r; r++) if (*r == '\r') *r = ' ';
        const char *s = skip_ws(line);
        if (!*s) continue;

        if (starts_with_ci(s, "org") && isspace((unsigned char)s[3])) {
            const char *v = skip_ws(s + 3);
            if (*v == '$') { c->org = (uint32_t)strtoul(v + 1, NULL, 16); c->have_org = 1; continue; }
        }
        if (starts_with_ci(s, "include") && isspace((unsigned char)s[7])) {
            Include inc;
            parse_path_and_group(s + 7, &inc.path, &inc.group);
            PUSH(c->inc, c->ninc, inc);
            continue;
        }
        if (starts_with_ci(s, "inclib") && isspace((unsigned char)s[6])) {
            char *path, *group;
            parse_path_and_group(s + 6, &path, &group);
            PUSH(c->libs, c->nlibs, path);
            c->lib_groups = xrealloc(c->lib_groups, sizeof(char *) * (size_t)c->nlibs);
            c->lib_groups[c->nlibs - 1] = group;
            continue;
        }
        /* "<name> group [kind] [file("...")]" */
        {
            const char *w = s;
            while (is_word((unsigned char)*w)) w++;
            if (w > s && isspace((unsigned char)*w)) {
                const char *g = skip_ws(w);
                if (starts_with_ci(g, "group") && !is_word((unsigned char)g[5])) {
                    Group gr = {0};
                    gr.name = xstrndup(s, (size_t)(w - s));
                    const char *rest = g + 5;
                    /* file("...") */
                    const char *f = rest;
                    char *kind = xstrdup(rest);
                    while (*f) {
                        if (starts_with_ci(f, "file(")) {
                            const char *q = skip_ws(f + 5);
                            if (*q == '"') {
                                const char *e = strchr(q + 1, '"');
                                if (e) {
                                    gr.file = xstrndup(q + 1, (size_t)(e - q - 1));
                                    /* cut the file(...) text out of the kind string */
                                    const char *close = strchr(e, ')');
                                    size_t from = (size_t)(f - rest), to = close ? (size_t)(close - rest) + 1 : strlen(rest);
                                    memmove(kind + from, kind + to, strlen(kind + to) + 1);
                                }
                            }
                            break;
                        }
                        f++;
                    }
                    /* trim the kind */
                    char *k = kind;
                    while (*k && isspace((unsigned char)*k)) k++;
                    size_t kl = strlen(k);
                    while (kl && isspace((unsigned char)k[kl - 1])) k[--kl] = 0;
                    gr.kind = kl ? xstrdup(k) : NULL;
                    PUSH(c->grp, c->ngrp, gr);
                    continue;
                }
            }
        }
        if (starts_with_ci(s, "section")) {
            const char *q = s + 7;
            int align = 0;
            if (*q == '.' && isdigit((unsigned char)q[1])) {
                align = (int)strtol(q + 1, (char **)&q, 10);
            }
            if (isspace((unsigned char)*q)) {
                q = skip_ws(q);
                const char *n = q;
                while (is_word((unsigned char)*q) || *q == '.') q++;
                if (q > n) {
                    char *name = xstrndup(n, (size_t)(q - n));
                    q = skip_ws(q);
                    if (*q == ',') {
                        q = skip_ws(q + 1);
                        const char *gs = q;
                        while (is_word((unsigned char)*q)) q++;
                        char *gname = xstrndup(gs, (size_t)(q - gs));
                        if (align) {
                            SAlign a = {name, align};
                            PUSH(c->sal, c->nsal, a);
                        }
                        for (int i = 0; i < c->ngrp; i++) {
                            if (!strcmp(c->grp[i].name, gname)) PUSH(c->grp[i].secs, c->grp[i].nsecs, name);
                        }
                        continue;
                    }
                }
            }
        }
        if (starts_with_ci(s, "regs") && isspace((unsigned char)s[4])) {
            const char *q = skip_ws(s + 4);
            if (starts_with_ci(q, "pc")) {
                q = skip_ws(q + 2);
                if (*q == '=') {
                    q = skip_ws(q + 1);
                    const char *n = q;
                    while (is_word((unsigned char)*q)) q++;
                    c->pc_sym = xstrndup(n, (size_t)(q - n));
                    continue;
                }
            }
        }
    }
    free(copy);
    return c;
}

static int cmd_align(const Cmd *c, const char *sname)
{
    for (int i = 0; i < c->nsal; i++)
        if (!strcmp(c->sal[i].name, sname)) return c->sal[i].align;
    return 0;
}

/* ------------------------------------------------------------------ paths */

/* absolute, normalised path; command files use backslashes as separators */
static char *norm_path(const char *p)
{
    char cwd[PATH_MAX];
    size_t pl = strlen(p);
    char *full;
    if (p[0] == '/') {
        full = xmalloc(pl + 2);
        memcpy(full, p, pl);
    } else {
        if (!getcwd(cwd, sizeof cwd)) die("getcwd failed");
        full = xmalloc(strlen(cwd) + pl + 3);
        sprintf(full, "%s/%s", cwd, p);
    }
    for (char *s = full; *s; s++) if (*s == '\\') *s = '/';
    char **part = xmalloc(sizeof(char *) * (strlen(full) / 2 + 2));
    int np = 0;
    for (char *t = strtok(full, "/"); t; t = strtok(NULL, "/")) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        part[np++] = t;
    }
    char *out = xmalloc(strlen(p) + PATH_MAX + 4);
    char *w = out;
    for (int i = 0; i < np; i++) w += sprintf(w, "/%s", part[i]);
    if (!np) strcpy(out, "/");
    return out;
}

/* ---------------------------------------------------------------- link */

typedef struct {
    Obj **o; int n;
} ObjList;

static void rename_sections(Obj *o, const char *group)
{
    /* an object included into group G contributes its ".text" to the group section "G.text", and so on */
    for (int i = 0; i < o->nsec; i++) {
        if (o->sec[i].name[0] == '.') {
            char *nn = xmalloc(strlen(group) + strlen(o->sec[i].name) + 1);
            sprintf(nn, "%s%s", group, o->sec[i].name);
            o->sec[i].name = nn;
        }
    }
}

static void absorb(const Obj *o, SMap *defined, SMap *undef)
{
    for (int i = 0; i < o->nxd; i++) { set_add(defined, o->xd[i].name); set_del(undef, o->xd[i].name); }
    for (int i = 0; i < o->ncm; i++) { set_add(defined, o->cm[i].name); set_del(undef, o->cm[i].name); }
    for (int i = 0; i < o->nxr; i++) if (!set_has(defined, o->xr[i].name)) set_add(undef, o->xr[i].name);
}

static ObjList link_objects(const Cmd *c, const char *libdir)
{
    ObjList L = {0};
    SMap defined, undef;
    smap_init(&defined);
    smap_init(&undef);

    for (int i = 0; i < c->ninc; i++) {
        char *fp = norm_path(c->inc[i].path);
        size_t len;
        uint8_t *d = read_file(fp, &len);
        Obj *o = parse_obj(fp, d, len);
        o->group = c->inc[i].group;
        if (o->group) rename_sections(o, o->group);
        PUSH(L.o, L.n, o);
    }
    for (int i = 0; i < L.n; i++) absorb(L.o[i], &defined, &undef);
    if (c->pc_sym && !set_has(&defined, c->pc_sym)) set_add(&undef, c->pc_sym);

    /* Library members are pulled on demand.  Observed behaviour (checked against the real tool): repeatedly scan
       the libraries in inclib order and each library's members in index order, pull the first member that exports
       a currently undefined symbol, and absorb its definitions and references immediately. */
    Lib **libs = xmalloc(sizeof(Lib *) * (size_t)(c->nlibs + 1));
    char **used = xmalloc(sizeof(char *) * (size_t)(c->nlibs + 1));
    for (int i = 0; i < c->nlibs; i++) {
        if (!libdir) die("inclib used but no /l library directory given");
        char *path = xmalloc(strlen(libdir) + strlen(c->libs[i]) + 2);
        sprintf(path, "%s/%s", libdir, c->libs[i]);
        libs[i] = load_lib(path);
        used[i] = xmalloc((size_t)libs[i]->nm + 1);
    }
    for (;;) {
        int pl = -1, pm = -1;
        for (int li = 0; li < c->nlibs && pl < 0; li++) {
            for (int mi = 0; mi < libs[li]->nm; mi++) {
                if (used[li][mi]) continue;
                const Member *m = &libs[li]->m[mi];
                for (int s = 0; s < m->nsyms; s++) {
                    if (set_has(&undef, m->syms[s])) { pl = li; pm = mi; break; }
                }
                if (pl >= 0) break;
            }
        }
        if (pl < 0) break;
        used[pl][pm] = 1;
        const Member *m = &libs[pl]->m[pm];
        char nm[512];
        snprintf(nm, sizeof nm, "%s(%s)", libs[pl]->path, m->name);
        Obj *o = parse_obj(nm, m->data, m->len);
        o->group = c->lib_groups[pl];
        if (o->group) rename_sections(o, o->group);
        PUSH(L.o, L.n, o);
        absorb(o, &defined, &undef);
    }
    return L;
}

/* ---------------------------------------------------------------- layout */

typedef struct {
    const char *sname, *gname;
    uint32_t a, b;           /* start, padded end */
    uint32_t real_end;       /* end of the last contribution, without the padding */
} Range;

typedef struct { uint32_t addr, size; const char *sname; } CommInfo;

typedef struct {
    Range *r; int nr;
    CommInfo *ci; int nci;
    SMap comm_addr;
} Layout;

/* C48 section ids: a plain section id, or 0xF000|id for library common blocks */
static const char *comm_section_name(const Obj *o, unsigned sid)
{
    const char *n = sec_name(o, sid);
    return n ? n : sec_name(o, sid & 0xff);
}

static unsigned name_sum(const char *s)
{
    unsigned h = 0;
    for (; *s; s++) h += (unsigned char)*s;
    return h;
}

static Layout do_layout(const Cmd *c, const ObjList *L)
{
    Layout ly = {0};
    smap_init(&ly.comm_addr);
    if (!c->have_org) die("no org in the command file");
    uint32_t addr = c->org;
    SMap defined;
    smap_init(&defined);
    for (int i = 0; i < L->n; i++)
        for (int k = 0; k < L->o[i]->nxd; k++) set_add(&defined, L->o[i]->xd[k].name);

    for (int gi = 0; gi < c->ngrp; gi++) {
        const Group *g = &c->grp[gi];
        for (int si = 0; si < g->nsecs; si++) {
            const char *sname = g->secs[si];
            int is_bss = !strcmp(sname, ".bss");
            if (is_bss) addr = (addr + 15) & ~15u;
            uint32_t start = addr, sec_begin = addr, total = 0;
            int al = cmd_align(c, sname);
            if (!al) al = is_bss ? 8 : 4;
            for (int oi = 0; oi < L->n; oi++) {
                Obj *o = L->o[oi];
                for (int k = 0; k < o->nsec; k++) {
                    if (strcmp(o->sec[k].name, sname)) continue;
                    uint32_t sz = sect_size(o, o->sec[k].id);
                    total += sz;
                    addr = (addr + (uint32_t)al - 1) & ~((uint32_t)al - 1);
                    o->sec[k].has_base = 1;
                    o->sec[k].base = addr;
                    addr += sz;
                }
                /* commons first declared by this object are allocated right after its own data, in hash-table
                   order: byte sum of the name modulo 509, ties newest first (chain head insertion) */
                int *ord = xmalloc(sizeof(int) * (size_t)(o->ncm + 1));
                for (int k = 0; k < o->ncm; k++) ord[k] = k;
                for (int a = 1; a < o->ncm; a++) {
                    int v = ord[a], b = a - 1;
                    for (; b >= 0; b--) {
                        unsigned ka = name_sum(o->cm[ord[b]].name) % 509, kv = name_sum(o->cm[v].name) % 509;
                        if (ka < kv || (ka == kv && ord[b] > v)) break;
                        ord[b + 1] = ord[b];
                    }
                    ord[b + 1] = v;
                }
                for (int a = 0; a < o->ncm; a++) {
                    const Comm *cm = &o->cm[ord[a]];
                    if (smap_get(&ly.comm_addr, cm->name, NULL) || set_has(&defined, cm->name)) continue;
                    const char *cs = comm_section_name(o, cm->sect);
                    if (!cs || strcmp(cs, sname)) continue;
                    addr = (addr + 3) & ~3u;
                    smap_put(&ly.comm_addr, cm->name, addr, 1);
                    CommInfo ci = {addr, (cm->size + 3) & ~3u, sname};   /* a common occupies whole words */
                    PUSH(ly.ci, ly.nci, ci);
                    addr += cm->size;
                    total += cm->size;
                }
                free(ord);
            }
            Range r = {sname, g->name, 0, 0, addr};
            if (total == 0) {
                /* nothing but empty contributions: only the alignment effect remains, no section row */
                r.a = r.b = addr;
                PUSH(ly.r, ly.nr, r);
                continue;
            }
            if (addr != sec_begin) addr = (addr + (uint32_t)al - 1) & ~((uint32_t)al - 1);
            r.a = start;
            r.b = addr;
            PUSH(ly.r, ly.nr, r);
        }
    }
    return ly;
}

static const Range *find_range(const Layout *ly, const char *sname)
{
    for (int i = 0; i < ly->nr; i++)
        if (!strcmp(ly->r[i].sname, sname)) return &ly->r[i];
    return NULL;
}

/* -------------------------------------------------------------- symbols */

static void def_sym(SMap *syms, const char *fmt, const char *a, uint32_t v)
{
    char nm[512];
    snprintf(nm, sizeof nm, fmt, a);
    smap_put(syms, nm, v, 0);                 /* first definition wins */
}

static void section_syms(SMap *syms, const char *key, uint32_t start, uint32_t tot, int group)
{
    const char *pre = group ? "_%s_" : "__%s_";
    static const char *suf[] = {"org", "obj", "orgend", "objend", "size"};
    uint32_t val[5] = {start, start, start + tot, start + tot, tot};
    for (int i = 0; i < 5; i++) {
        char fmt[64];
        snprintf(fmt, sizeof fmt, "%s%s", pre, suf[i]);
        def_sym(syms, fmt, key, val[i]);
    }
}

static SMap symbol_table(const Cmd *c, const ObjList *L, const Layout *ly)
{
    SMap syms;
    smap_init(&syms);
    for (int oi = 0; oi < L->n; oi++) {
        const Obj *o = L->o[oi];
        for (int k = 0; k < o->nxd; k++) {
            const char *sn = sec_name(o, o->xd[k].sect);
            uint32_t b;
            if (sn && sec_base(o, sn, &b)) smap_put(&syms, o->xd[k].name, b + o->xd[k].off, 0);
        }
    }
    for (size_t i = 0; i < ly->comm_addr.cap; i++)
        if (ly->comm_addr.keys[i]) smap_put(&syms, ly->comm_addr.keys[i], ly->comm_addr.vals[i], 0);

    /* per-section symbols: __<sect>_org/obj/orgend/objend/size (size = sum of contributions, commons included) */
    for (int ri = 0; ri < ly->nr; ri++) {
        const Range *r = &ly->r[ri];
        uint32_t tot = 0;
        int have_first = 0;
        uint32_t first = 0;
        for (int oi = 0; oi < L->n; oi++) {
            const Obj *o = L->o[oi];
            for (int k = 0; k < o->nsec; k++) {
                if (strcmp(o->sec[k].name, r->sname) || !o->sec[k].has_base) continue;
                tot += sect_size(o, o->sec[k].id);
            }
            uint32_t b;
            if (sec_base(o, r->sname, &b) && (!have_first || b < first)) { first = b; have_first = 1; }
        }
        SMap seen;
        smap_init(&seen);
        for (int oi = 0; oi < L->n; oi++) {
            const Obj *o = L->o[oi];
            for (int k = 0; k < o->ncm; k++) {
                const char *cs = comm_section_name(o, o->cm[k].sect);
                if (smap_get(&ly->comm_addr, o->cm[k].name, NULL) && !set_has(&seen, o->cm[k].name) && cs && !strcmp(cs, r->sname)) {
                    set_add(&seen, o->cm[k].name);
                    tot += o->cm[k].size;
                }
            }
        }
        const char *key = r->sname;
        while (*key == '.') key++;
        uint32_t start = (tot && have_first) ? first : (tot ? r->a : r->a);
        if (!tot) start = r->a;
        section_syms(&syms, key, start, tot, 0);
    }
    /* per-group symbols: _<group>_org/obj/orgend/objend/size */
    for (int gi = 0; gi < c->ngrp; gi++) {
        int any = 0;
        uint32_t lo = 0, hi = 0;
        for (int ri = 0; ri < ly->nr; ri++) {
            if (strcmp(ly->r[ri].gname, c->grp[gi].name)) continue;
            if (!any) lo = ly->r[ri].a;
            hi = ly->r[ri].b;
            any = 1;
        }
        if (any) section_syms(&syms, c->grp[gi].name, lo, hi - lo, 1);
    }
    const Range *sd = find_range(ly, ".sdata");
    if (sd) smap_put(&syms, "__SN_GP_BASE", sd->a, 0);
    return syms;
}

/* ---------------------------------------------------------- relocations */

static uint32_t eval_expr(const Obj *o, int ei, const SMap *syms, const Layout *ly)
{
    const Expr *e = &o->ex[ei];
    switch (e->kind) {
    case E_CONST:
        return e->val;
    case E_SYM: {
        const char *nm = NULL;
        for (int i = 0; i < o->nxr; i++) if (o->xr[i].id == e->val) nm = o->xr[i].name;
        if (!nm) {
            for (int i = 0; i < o->nxd; i++) if (o->xd[i].id == e->val) nm = o->xd[i].name;
            for (int i = 0; i < o->ncm; i++) if (o->cm[i].id == e->val) nm = o->cm[i].name;
        }
        uint32_t v;
        if (!nm || !smap_get(syms, nm, &v)) die("undefined symbol %s", nm ? nm : "(unnamed)");
        return v;
    }
    case E_SEC: {
        const char *sn = sec_name(o, e->val);
        uint32_t b;
        if (!sn || !sec_base(o, sn, &b)) die("%s: section %u was not laid out", o->path, e->val);
        return b;
    }
    case E_X: {
        const char *sn = sec_name(o, e->val);
        const Range *r = sn ? find_range(ly, sn) : NULL;
        if (!r) die("%s: no linked section for node %d", o->path, e->xtype);
        if (e->xtype == 12) return r->a;        /* start of the linked section */
        if (e->xtype == 22) return r->b;        /* end of the linked section */
        die("%s: unsupported expression node %d", o->path, e->xtype);
    }
    default:
        break;
    }
    uint32_t x = eval_expr(o, e->a, syms, ly), y = eval_expr(o, e->b, syms, ly);
    switch (e->val) {
    case 44: return x + y;
    case 46: return y - x;
    case 52: return y & x;
    case 60: return x >= 32 ? 0 : y >> x;
    case 50: return x ? y / x : 0;
    }
    die("%s: unsupported expression operator %u", o->path, e->val);
}

static uint32_t apply_reloc(unsigned ty, uint32_t word, uint32_t v, uint32_t gp)
{
    switch (ty) {
    case 74: return (word & 0xfc000000u) | ((v >> 2) & 0x03ffffffu);
    case 82: return (word & 0xffff0000u) | (((v + 0x8000u) >> 16) & 0xffffu);
    case 84: return (word & 0xffff0000u) | (v & 0xffffu);
    case 16: return v;
    case 100: return (word & 0xffff0000u) | ((v - gp) & 0xffffu);
    case 26: case 30: return (word & 0xffff0000u) | (v & 0xffffu);
    }
    die("unsupported relocation type %u", ty);
}

typedef struct {
    uint32_t addr;
    uint8_t *data;
    uint32_t len;
    const char *sname;
} Chunk;

typedef struct {
    Chunk *c; int n;          /* loaded data: object CODE records, ZEROES in loadable sections, commons */
    Chunk *bss; int nbss;     /* CODE records inside bss-group sections (only written by the fopen-mod linker) */
} Chunks;

static int in_list(char **l, int n, const char *s)
{
    for (int i = 0; i < n; i++) if (!strcmp(l[i], s)) return 1;
    return 0;
}

static Chunks build_chunks(const ObjList *L, const SMap *syms, const Layout *ly, char **bss_names, int nbss_names)
{
    Chunks ck = {0};
    uint32_t gp = 0;
    smap_get(syms, "__SN_GP_BASE", &gp);
    for (int oi = 0; oi < L->n; oi++) {
        const Obj *o = L->o[oi];
        long cur = -1;
        uint32_t *loc = xmalloc(sizeof(uint32_t) * (size_t)(o->nsec + 1));
        int i = 0;
        while (i < o->nit) {
            const Item *it = &o->it[i];
            if (it->kind == I_SWITCH) {
                cur = it->n;
            } else if (it->kind == I_ZEROES) {
                if (cur < 0) die("%s: data before the first section", o->path);
                const char *sn = sec_name(o, (unsigned)cur);
                uint32_t b;
                int si = -1;
                for (int k = 0; k < o->nsec; k++) if (o->sec[k].id == cur) si = k;
                if (sn && sec_base(o, sn, &b)) {
                    Chunk c = {b + loc[si], xmalloc(it->n), it->n, sn};
                    PUSH(ck.c, ck.n, c);
                }
                loc[si] += it->n;
            } else if (it->kind == I_CODE) {
                if (cur < 0) die("%s: data before the first section", o->path);
                int si = -1;
                for (int k = 0; k < o->nsec; k++) if (o->sec[k].id == cur) si = k;
                uint8_t *data = xmalloc(it->n);
                memcpy(data, it->data, it->n);
                int j = i + 1;
                while (j < o->nit && o->it[j].kind == I_RELOC) {
                    const Item *r = &o->it[j];
                    uint32_t v = eval_expr(o, r->expr, syms, ly);
                    if ((size_t)r->at + 4 > it->n) die("%s: relocation outside its code record", o->path);
                    uint32_t w = rd32(data + r->at);
                    w = apply_reloc(r->n, w, v, gp);
                    data[r->at] = (uint8_t)w; data[r->at + 1] = (uint8_t)(w >> 8);
                    data[r->at + 2] = (uint8_t)(w >> 16); data[r->at + 3] = (uint8_t)(w >> 24);
                    j++;
                }
                const char *sn = sec_name(o, (unsigned)cur);
                uint32_t b;
                if (sn && sec_base(o, sn, &b)) {
                    Chunk c = {b + loc[si], data, it->n, sn};
                    if (!in_list(bss_names, nbss_names, sn)) PUSH(ck.c, ck.n, c);
                    else PUSH(ck.bss, ck.nbss, c);
                }
                loc[si] += it->n;
                i = j;
                continue;
            }
            i++;
        }
        free(loc);
    }
    for (int i = 0; i < ly->nci; i++) {
        Chunk c = {ly->ci[i].addr, xmalloc(ly->ci[i].size), ly->ci[i].size, ly->ci[i].sname};
        PUSH(ck.c, ck.n, c);
    }
    return ck;
}

/* --------------------------------------------------------------- writers */

static void write_cpe(const char *path, const Chunk *c, int n, uint32_t pc)
{
    Buf b = {0};
    bput(&b, "CPE\x01", 4);
    bput(&b, "\x08\x00", 2);                     /* select unit 0 */
    bput(&b, "\x03\x90\x00", 3);                 /* register 0x90 (PC) */
    b32(&b, pc);
    for (int i = 0; i < n; i++) {
        b8(&b, 1);
        b32(&b, c[i].addr);
        b32(&b, c[i].len);
        bput(&b, c[i].data, c[i].len);
    }
    b8(&b, 0);
    write_file(path, b.p, b.n);
}

typedef struct { const char *name; uint32_t addr; } SymEnt;

static int cmp_name(const void *a, const void *b)
{
    return strcmp(((const SymEnt *)a)->name, ((const SymEnt *)b)->name);
}

static int cmp_addr(const void *a, const void *b)
{
    const SymEnt *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void write_map(const char *path, const Layout *ly, const SMap *syms, const ObjList *L)
{
    Buf b = {0};
    char line[1024];
#define LINE(...) do { int n_ = snprintf(line, sizeof line, __VA_ARGS__); bput(&b, line, (size_t)n_); bput(&b, "\r\n", 2); } while (0)
    LINE("%s", "");
    LINE("    Start     Stop   Length      Obj Group            Section name");
    for (int i = 0; i < ly->nr; i++) {
        const Range *r = &ly->r[i];
        if (r->b <= r->a) continue;
        uint32_t first = r->a, v;
        int have = 0;
        for (int oi = 0; oi < L->n; oi++) {
            if (sec_base(L->o[oi], r->sname, &v) && (!have || v < first)) { first = v; have = 1; }
        }
        LINE(" %08X %08X %08X %08X %-16s %s", r->a, r->b - 1, r->b - r->a, first, r->gname, r->sname);
    }
    LINE("%s", "");
    LINE("Program entry point : 00000000");
    LINE("%s", "");
    LINE("%s", "");
    LINE("  Address  Names alphabetically");
    SymEnt *e = xmalloc(sizeof(SymEnt) * (syms->n + 1));
    size_t n = 0;
    for (size_t i = 0; i < syms->cap; i++) if (syms->keys[i]) { e[n].name = syms->keys[i]; e[n].addr = syms->vals[i]; n++; }
    qsort(e, n, sizeof *e, cmp_name);
    for (size_t i = 0; i < n; i++) LINE(" %08X  %s", e[i].addr, e[i].name);
    LINE("%s", "");
    LINE("%s", "");
    LINE("%s", "");
    LINE("  Address  Names in address order");
    qsort(e, n, sizeof *e, cmp_addr);
    for (size_t i = 0; i < n; i++) LINE(" %08X  %s", e[i].addr, e[i].name);
    LINE("%s", "");
#undef LINE
    write_file(path, b.p, b.n);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* --------------------------------------------------------------- driver */

static char *trim_quotes(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

int main(int argc, char **argv)
{
    const char *libdir = NULL;
    char *spec = NULL;
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int fopen_mod = strstr(prog, "fopen_mod") != NULL;

    for (int i = 1; i < argc; i++) {
        char low[16];
        size_t l = strlen(argv[i]);
        for (size_t k = 0; k < sizeof low - 1; k++) low[k] = k < l ? (char)tolower((unsigned char)argv[i][k]) : 0;
        low[sizeof low - 1] = 0;
        if (!strcmp(low, "/l") && i + 1 < argc) { libdir = argv[++i]; continue; }
        if (!strcmp(low, "/c") || !strcmp(low, "/q") || !strcmp(low, "/m")) continue;
        if ((!strcmp(low, "/n") || !strcmp(low, "/gp")) && i + 1 < argc) { i++; continue; }
        spec = argv[i];
    }
    if (!spec) {
        fprintf(stderr, "usage: psylink [options] @cmdfile,out.cpe[,out.sym[,out.map]]\n");
        return 1;
    }
    char *parts[4] = {0};
    int np = 0;
    for (char *s = spec;; ) {
        if (np < 4) parts[np++] = s;
        char *comma = strchr(s, ',');
        if (!comma) break;
        *comma = 0;
        s = comma + 1;
    }
    char *cmdfile = trim_quotes(parts[0]);
    while (*cmdfile == '"' || *cmdfile == '@') cmdfile++;
    { size_t n = strlen(cmdfile); while (n && cmdfile[n - 1] == '"') cmdfile[--n] = 0; }

    size_t tl;
    uint8_t *text = read_file(cmdfile, &tl);
    Cmd *cmd = parse_cmd((const char *)text);
    ObjList L = link_objects(cmd, libdir);
    Layout ly = do_layout(cmd, &L);

    for (int i = 0; i < ly.nr; i++)
        printf("%08X %08X %08X %-6s %s\n", ly.r[i].a, ly.r[i].b - 1, ly.r[i].b - ly.r[i].a, ly.r[i].gname, ly.r[i].sname);

    SMap syms = symbol_table(cmd, &L, &ly);

    /* sections that are zero-initialised memory: the bss-kind groups and anything called *sbss / *.bss */
    char **bss_names = NULL;
    int nbss_names = 0;
    for (int gi = 0; gi < cmd->ngrp; gi++) {
        for (int si = 0; si < cmd->grp[gi].nsecs; si++) {
            const char *s = cmd->grp[gi].secs[si];
            size_t sl = strlen(s);
            int by_kind = cmd->grp[gi].kind && !strcasecmp(cmd->grp[gi].kind, "bss");
            int by_name = (sl >= 4 && !strcmp(s + sl - 4, "sbss")) || (sl >= 4 && !strcmp(s + sl - 4, ".bss"));
            if (by_kind || by_name) PUSH(bss_names, nbss_names, (char *)s);
        }
    }
    Chunks ck = build_chunks(&L, &syms, &ly, bss_names, nbss_names);

    char **file_secs = NULL;
    int nfile_secs = 0;
    for (int gi = 0; gi < cmd->ngrp; gi++)
        if (cmd->grp[gi].file)
            for (int si = 0; si < cmd->grp[gi].nsecs; si++) PUSH(file_secs, nfile_secs, cmd->grp[gi].secs[si]);

    if (cmd->pc_sym) {
        uint32_t pc;
        if (!smap_get(&syms, cmd->pc_sym, &pc)) die("undefined symbol %s", cmd->pc_sym);
        Chunk *sel = xmalloc(sizeof(Chunk) * (size_t)(ck.n + 1));
        int ns = 0;
        for (int i = 0; i < ck.n; i++)
            if (!in_list(file_secs, nfile_secs, ck.c[i].sname) && !in_list(bss_names, nbss_names, ck.c[i].sname)) sel[ns++] = ck.c[i];
        write_cpe(np > 1 ? trim_quotes(parts[1]) : "out.cpe", sel, ns, pc);
    }

    for (int gi = 0; gi < cmd->ngrp; gi++) {
        const Group *g = &cmd->grp[gi];
        if (!g->file) continue;
        int have = 0;
        uint32_t lo = 0;
        for (int ri = 0; ri < ly.nr; ri++) {
            if (!strcmp(ly.r[ri].gname, g->name) && ly.r[ri].b > ly.r[ri].a) { lo = ly.r[ri].a; have = 1; break; }
        }
        if (!have) continue;
        Chunk *mine = NULL;
        int nm = 0;
        for (int i = 0; i < ck.n; i++) if (in_list(g->secs, g->nsecs, ck.c[i].sname)) PUSH(mine, nm, ck.c[i]);
        char *path = norm_path(g->file);
        Buf img = {0};
        if (fopen_mod && file_exists(path)) {
            /* psylink_overlay_fopen_mod: write the chunks on top of the existing file, leave the rest alone */
            for (int i = 0; i < ck.nbss; i++) if (in_list(g->secs, g->nsecs, ck.bss[i].sname)) PUSH(mine, nm, ck.bss[i]);
            size_t flen;
            uint8_t *old = read_file(path, &flen);
            bput(&img, old, flen);
        } else {
            uint32_t hi = lo;
            for (int i = 0; i < nm; i++) if (mine[i].addr + mine[i].len > hi) hi = mine[i].addr + mine[i].len;   /* ends at the last chunk written */
            uint8_t *z = xmalloc(hi - lo);
            bput(&img, z, hi - lo);
        }
        for (int i = 0; i < nm; i++) {
            size_t off = mine[i].addr - lo;
            if (off + mine[i].len > img.n) {
                size_t grow = off + mine[i].len - img.n;
                uint8_t *z = xmalloc(grow);
                bput(&img, z, grow);
            }
            memcpy(img.p + off, mine[i].data, mine[i].len);
        }
        write_file(path, img.p, img.n);
    }

    if (np > 3) write_map(trim_quotes(parts[3]), &ly, &syms, &L);

    if (getenv("PSYLINK_DUMP")) {
        SymEnt *e = xmalloc(sizeof(SymEnt) * (syms.n + 1));
        size_t n = 0;
        for (size_t i = 0; i < syms.cap; i++) if (syms.keys[i]) { e[n].name = syms.keys[i]; e[n].addr = syms.vals[i]; n++; }
        qsort(e, n, sizeof *e, cmp_name);
        for (size_t i = 0; i < n; i++) printf("%08X  %s\n", e[i].addr, e[i].name);
    }
    return 0;
}
