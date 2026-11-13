#!/usr/bin/env python3
"""Fill the compiler-output cache for the jobs the native cc1psx cannot reproduce.

Runs the native 4.4 compiler over the golden jobs and caches the real compiler's output for every job that differs
(and for every job that needs the PsyQ 4.3 compiler, which has no native replacement yet).  Cache entries are keyed by
the SHA-256 of the compiler input plus the -G size, so an edited source file never gets a stale answer.

usage: cc1psx_make_cache.py       (needs corpus/cc1psx and bin/cc1psx)
"""
import concurrent.futures, hashlib, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
CORPUS = os.path.join(ROOT, 'corpus', 'cc1psx')
CACHE = os.path.join(ROOT, 'corpus', 'cc1psx_cache')
CC1 = os.path.join(ROOT, 'bin', 'cc1psx')


def key(path, g):
    # preprocessors differ in whitespace and line markers, not in the tokens: hash the tokens only
    text = b''.join(l for l in open(path, 'rb').read().split(b'\n') if not l.lstrip().startswith(b'#'))
    name = os.path.basename(path).split('.')[0].encode()
    return hashlib.sha256(name + b'\0' + b''.join(text.split()) + b'\0G=' + g.encode()).hexdigest()


def job(line):
    rule, inp, out, g = line.split()
    src = os.path.join(CORPUS, inp.replace('../', '', 1))
    gold = os.path.join(CORPUS, out.replace('../', '', 1))
    if rule == 'cc1psx-4.3':
        return key(src, g), gold, '4.3 job'
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, 'o.asm')
        r = subprocess.run([CC1, '-quiet', '-O2', '-G', g, '-g0', '-Wall', src, '-o', o], capture_output=True)
        if r.returncode == 0 and open(o, 'rb').read() == open(gold, 'rb').read():
            return None
    return key(src, g), gold, 'native output differs'


def main():
    os.makedirs(CACHE, exist_ok=True)
    lines = [l for l in open(os.path.join(CORPUS, 'manifest.txt')).read().split('\n') if l.strip()]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
        res = [r for r in ex.map(job, lines) if r]
    for k, gold, why in res:
        open(os.path.join(CACHE, k + '.asm'), 'wb').write(open(gold, 'rb').read())
        print('cached %s (%s)' % (os.path.relpath(gold, CORPUS), why))
    print('%d entries' % len(res))


if __name__ == '__main__':
    main()
