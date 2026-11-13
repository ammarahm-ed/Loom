#!/usr/bin/env python3
"""Turns `cmd package dump-profiles` output into baseline-profile rules.

    python3 bench/profile-to-baseline.py <prefix> <dump.prof.txt>... > rules.txt

Keeps every method the profile saw run (hot, startup and post-startup), and their classes, whose class starts with
<prefix> (e.g. com.facebook.react). Output is in baseline-prof.txt format.
"""
import re
import sys

PRIM = {'void': 'V', 'boolean': 'Z', 'byte': 'B', 'short': 'S', 'char': 'C',
        'int': 'I', 'long': 'J', 'float': 'F', 'double': 'D'}


def desc(t):
    t = t.strip()
    dims = 0
    while t.endswith('[]'):
        dims += 1
        t = t[:-2]
    base = PRIM.get(t) or ('L' + t.replace('.', '/') + ';')
    return '[' * dims + base


def split_args(s):
    return [a for a in (x.strip() for x in s.split(',')) if a]


def main():
    prefix = sys.argv[1]
    out_methods, out_classes = set(), set()
    pat = re.compile(r'^\s*(\S+) ([\w.$]+)\.([\w$<>]+)\((.*)\)(?:\[[^\]]*\])?,?\s*$')
    for path in sys.argv[2:]:
        section = None
        for line in open(path, errors='replace'):
            stripped = line.strip()
            if stripped.endswith(':') and not stripped.startswith(('void ', 'int ')):
                section = stripped[:-1]
                continue
            if section not in ('hot methods', 'startup methods', 'post startup methods'):
                continue
            m = pat.match(line)
            if not m:
                continue
            ret, cls, name, args = m.groups()
            if not cls.startswith(prefix):
                continue
            cdesc = 'L' + cls.replace('.', '/') + ';'
            sig = '(' + ''.join(desc(a) for a in split_args(args)) + ')' + desc(ret)
            out_methods.add(f'HSP{cdesc}->{name}{sig}')
            out_classes.add(cdesc)
    for c in sorted(out_classes):
        print(c)
    for m in sorted(out_methods):
        print(m)


if __name__ == '__main__':
    main()
