#!/usr/bin/env python3
"""Dump a PsyQ/SN 'LNK' version 2 object file as one text line per record.

Record layouts follow what can be observed in real objects and libraries.  Unknown tags raise, so a corpus-wide
run proves the grammar covers every file.

usage: lnkdump.py file.obj [file2.obj]   (two files: print a diff)
"""
import struct
import sys
import difflib


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v, = struct.unpack_from('<H', self.d, self.p)
        self.p += 2
        return v

    def u32(self):
        v, = struct.unpack_from('<I', self.d, self.p)
        self.p += 4
        return v

    def raw(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def pstr(self):
        return self.raw(self.u8()).decode('latin-1')

    def eof(self):
        return self.p >= len(self.d)


def expr(r):
    """Relocation / fixup expression tree rendered as text."""
    t = r.u8()
    if t == 0:
        return '#%08x' % r.u32()
    if t in (2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24):
        return 'op%d(%04x)' % (t, r.u16())
    a = expr(r)
    b = expr(r)
    return 'op%d(%s,%s)' % (t, a, b)


def records(data):
    r = Reader(data)
    if r.raw(3) != b'LNK' or r.u8() != 2:
        raise ValueError('not an LNK v2 file')
    out = []
    while not r.eof():
        off = r.p
        c = r.u8()
        if c == 0:
            out.append((off, 'END'))
            break
        elif c == 2:
            n = r.u16()
            out.append((off, 'CODE %d %s' % (n, r.raw(n).hex())))
        elif c == 6:
            out.append((off, 'SWITCH %d' % r.u16()))
        elif c == 8:
            out.append((off, 'ZEROES %d' % r.u32()))
        elif c == 10:
            ty = r.u8()
            at = r.u16()
            out.append((off, 'RELOC type=%d at=%04x %s' % (ty, at, expr(r))))
        elif c == 12:
            n = r.u16(); s = r.u16(); o = r.u32()
            out.append((off, 'XDEF #%d sect=%d off=%08x %s' % (n, s, o, r.pstr())))
        elif c == 14:
            n = r.u16()
            out.append((off, 'XREF #%d %s' % (n, r.pstr())))
        elif c == 16:
            i = r.u16(); g = r.u16(); al = r.u8()
            out.append((off, 'SECTION %d group=%d align=%d %s' % (i, g, al, r.pstr())))
        elif c == 18:
            s = r.u16(); o = r.u32()
            out.append((off, 'LOCAL sect=%d off=%08x %s' % (s, o, r.pstr())))
        elif c == 20:
            n = r.u16(); ty = r.u8()
            out.append((off, 'GROUP #%d type=%d %s' % (n, ty, r.pstr())))
        elif c == 28:
            n = r.u16()
            out.append((off, 'FILE %d %s' % (n, r.pstr())))
        elif c == 46:
            out.append((off, 'CPU %d' % r.u8()))
        elif c == 48:
            a = r.u16(); b = r.u16(); o = r.u32()
            out.append((off, 'C48 %d %d %08x %s' % (a, b, o, r.pstr())))
        elif c == 50:
            out.append((off, 'C50 %d' % r.u16()))
        elif c == 52:
            out.append((off, 'C52 %d %d' % (r.u16(), r.u8())))
        elif c == 54:
            out.append((off, 'SLD_INC %d %04x' % (r.u16(), r.u16())))
        elif c == 56:
            out.append((off, 'SLD_SET %d %08x' % (r.u16(), r.u32())))
        elif c == 58:
            out.append((off, 'SLD58 %d %08x %d' % (r.u16(), r.u32(), r.u16())))
        elif c == 60:
            out.append((off, 'SLD60 %d' % r.u16()))
        elif c == 74:
            f = [r.u16(), r.u32(), r.u16(), r.u32(), r.u16(), r.u32(), r.u16(), r.u32(), r.u32()]
            out.append((off, 'FUNC %s %s' % (' '.join('%x' % x for x in f), r.pstr())))
        elif c == 76:
            out.append((off, 'FUNC_END %d %08x %08x' % (r.u16(), r.u32(), r.u32())))
        elif c == 78:
            out.append((off, 'BLOCK_START %d %08x %08x' % (r.u16(), r.u32(), r.u32())))
        elif c == 80:
            out.append((off, 'BLOCK_END %d %08x %08x' % (r.u16(), r.u32(), r.u32())))
        elif c == 82:
            f = [r.u16(), r.u32(), r.u16(), r.u16(), r.u32()]
            out.append((off, 'DEF %s %s' % (' '.join('%x' % x for x in f), r.pstr())))
        elif c == 84:
            f = [r.u16(), r.u32(), r.u16(), r.u16(), r.u32()]
            dims = [r.u32() for _ in range(r.u16())]
            tag = r.pstr()
            out.append((off, 'DEF2 %s dims=%s tag=%s %s' % (
                ' '.join('%x' % x for x in f), dims, tag, r.pstr())))
        else:
            raise ValueError('unknown tag %d (0x%x) at offset 0x%x' % (c, c, off))
    return out


def dump(path):
    with open(path, 'rb') as f:
        data = f.read()
    return ['%06x %s' % rec for rec in records(data)]


def main(argv):
    if len(argv) == 2:
        print('\n'.join(dump(argv[1])))
        return 0
    if len(argv) == 3:
        a, b = dump(argv[1]), dump(argv[2])
        # strip offsets so inserted/removed records do not shift every line
        strip = lambda ls: [l.split(' ', 1)[1] for l in ls]
        diff = list(difflib.unified_diff(strip(a), strip(b), argv[1], argv[2], lineterm='', n=2))
        print('\n'.join(diff) if diff else 'identical records')
        return 1 if diff else 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
