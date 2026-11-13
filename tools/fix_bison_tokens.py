#!/usr/bin/env python3
"""Turn the `enum yytokentype { A = 258, ... };` blocks that modern bison writes into plain #defines,
like the old bison GCC 2.8.1 expects (the enum names clash with rtl.def's RETURN, etc.)."""
import re, sys
for f in sys.argv[1:]:
    t = open(f).read()
    def repl(m):
        body = m.group(1)
        return ''.join('#define %s %s\n' % (n, v) for n, v in re.findall(r'(\w+)\s*=\s*(\d+)', body))
    t2 = re.sub(r'enum\s+yytokentype\s*\{(.*?)\};', repl, t, flags=re.S)
    if t2 != t:
        open(f, 'w').write(t2)
        print('fixed', f)
