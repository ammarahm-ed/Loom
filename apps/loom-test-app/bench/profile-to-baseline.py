#!/usr/bin/env python3
"""Converts an ART profile dump into baseline-profile rules (the human-readable format AGP reads).

    adb shell cmd package dump-profiles <package>
    adb pull /data/misc/profman/<package>-primary.prof.txt
    python3 bench/profile-to-baseline.py <dump.txt> [prefix ...] > baseline-prof.txt

`dump-profiles` prints Java signatures grouped as hot / startup / post startup methods plus loaded
classes. Each is turned into a rule (`HSPLcom/x/Y;->m(I)V`, `Lcom/x/Y;`). Only classes under the
given package prefixes are kept (default: com.facebook.), so the rules describe React Native itself
and can ship inside the ReactAndroid library, where AGP merges them into every app's profile.
"""
import re
import sys

PRIMITIVES = {
    'void': 'V', 'boolean': 'Z', 'byte': 'B', 'char': 'C', 'short': 'S',
    'int': 'I', 'long': 'J', 'float': 'F', 'double': 'D',
}


def descriptor(java_type):
    java_type = java_type.strip()
    dims = 0
    while java_type.endswith('[]'):
        dims += 1
        java_type = java_type[:-2]
    base = PRIMITIVES.get(java_type) or 'L' + java_type.replace('.', '/') + ';'
    return '[' * dims + base


def split_params(params):
    return [p for p in (x.strip() for x in params.split(',')) if p]


METHOD = re.compile(r'^(\S+) ([^\s(]+)\.([^.(\s]+)\(([^)]*)\)')


def main():
    path, prefixes = sys.argv[1], tuple(sys.argv[2:]) or ('com.facebook.',)
    flags = {}
    classes = set()
    section = None
    for raw in open(path, encoding='utf-8', errors='replace'):
        line = raw.strip()
        if line.startswith('=== reference profile'):
            break
        if line.endswith('methods:') or line == 'classes:':
            section = {'hot methods:': 'H', 'startup methods:': 'S',
                       'post startup methods:': 'P', 'classes:': 'C'}.get(line)
            continue
        if not line or section is None or '[index=' in line:
            continue
        line = line.rstrip(',').strip()
        if section == 'C':
            if line.startswith(prefixes):
                classes.add(descriptor(line))
            continue
        m = METHOD.match(line)
        if not m:
            continue
        ret, owner, name, params = m.groups()
        if not owner.startswith(prefixes):
            continue
        rule = (f'{descriptor(owner)}->{name}('
                + ''.join(descriptor(p) for p in split_params(params)) + f'){descriptor(ret)}')
        flags.setdefault(rule, set()).add(section)
        classes.add(descriptor(owner))
    for rule in sorted(flags):
        print(''.join(f for f in 'HSP' if f in flags[rule]) + rule)
    for cls in sorted(classes):
        print(cls)


if __name__ == '__main__':
    main()
