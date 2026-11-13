/*
 * cpe2exe - re-implementation of PsyQ's CPE2EXE (CPE image -> PlayStation PS-X EXE).
 *
 * usage: cpe2exe [-CJ|-CE|-CU] input.cpe [output.exe]
 *
 * Clean-room: the layout was derived by comparing real converter output with its input.  The image runs from
 * the lowest to the highest loaded address (gaps are zero), is padded to a multiple of 0x800 and placed after a
 * 0x800 byte header holding the entry point, load address, size, initial stack pointer and region string.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "cpe2exe: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(int argc, char **argv)
{
    const char *region = "Sony Computer Entertainment Inc. for Japan area";
    const char *in = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'J') region = "Sony Computer Entertainment Inc. for Japan area";
                else if (*f == 'E') region = "Sony Computer Entertainment Inc. for Europe area";
                else if (*f == 'U') region = "Sony Computer Entertainment Inc. for North America area";
            }
        } else if (!in) {
            in = argv[i];
        } else {
            out = argv[i];
        }
    }
    if (!in) {
        fprintf(stderr, "usage: cpe2exe [-CJ|-CE|-CU] input.cpe [output.exe]\n");
        return 1;
    }

    FILE *f = fopen(in, "rb");
    if (!f) die("cannot open", in);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len + 1);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) die("cannot read", in);
    fclose(f);
    if (len < 5 || memcmp(d, "CPE\x01", 4)) die("not a CPE file", in);

    /* first pass: extent of the loaded data and the registers */
    uint32_t lo = 0xffffffffu, hi = 0, pc = 0, gp = 0;
    size_t p = 4;
    for (;;) {
        if (p >= (size_t)len) die("truncated CPE file", in);
        unsigned t = d[p++];
        if (t == 0) break;
        if (t == 1) {
            if (p + 8 > (size_t)len) die("truncated CPE file", in);
            uint32_t a = rd32(d + p), n = rd32(d + p + 4);
            p += 8;
            if (p + n > (size_t)len) die("truncated CPE file", in);
            if (a < lo) lo = a;
            if (a + n > hi) hi = a + n;
            p += n;
        } else if (t == 3) {
            if (p + 6 > (size_t)len) die("truncated CPE file", in);
            unsigned reg = d[p] | d[p + 1] << 8;
            uint32_t v = rd32(d + p + 2);
            p += 6;
            if (reg == 0x90) pc = v;
            if (reg == 0x8f) gp = v;
        } else if (t == 8) {
            p += 1;
        } else {
            die("unsupported CPE record", in);
        }
    }
    if (lo == 0xffffffffu) die("CPE file has no data", in);

    size_t span = hi - lo, image = (span + 0x7ff) & ~(size_t)0x7ff;
    uint8_t *exe = calloc(0x800 + image, 1);
    if (!exe) die("out of memory", NULL);
    memcpy(exe, "PS-X EXE", 8);
    wr32(exe + 0x10, pc);
    wr32(exe + 0x14, gp);
    wr32(exe + 0x18, lo);
    wr32(exe + 0x1c, (uint32_t)image);
    wr32(exe + 0x30, 0x801ffff0u);               /* initial stack pointer */
    memcpy(exe + 0x4c, region, strlen(region));

    /* second pass: copy the data */
    p = 4;
    for (;;) {
        unsigned t = d[p++];
        if (t == 0) break;
        if (t == 1) {
            uint32_t a = rd32(d + p), n = rd32(d + p + 4);
            p += 8;
            memcpy(exe + 0x800 + (a - lo), d + p, n);
            p += n;
        } else if (t == 3) {
            p += 6;
        } else if (t == 8) {
            p += 1;
        }
    }

    char buf[4096];
    if (!out) {
        size_t n = strlen(in);
        if (n > 4 && !strcmp(in + n - 4, ".cpe")) n -= 4;
        snprintf(buf, sizeof buf, "%.*s.exe", (int)n, in);
        out = buf;
    }
    f = fopen(out, "wb");
    if (!f || fwrite(exe, 1, 0x800 + image, f) != 0x800 + image) die("cannot write", out);
    fclose(f);
    return 0;
}
