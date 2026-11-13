#!/usr/bin/env python3
"""Top functions inside each language group of a bench/profile.sh recording.

    python3 bench/profile-drilldown.py <scenario> <variant> [top]

For each group of profile-languages.py, lists the functions where the samples landed (self time).
For libc samples (malloc, memcpy, free...) it also lists the first caller outside libc, which is
the code that asked for the allocation or copy.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module  # noqa: E402

langs = import_module('profile-languages')
ReportLib, OUT, SAMPLE_HZ = langs.ReportLib, langs.OUT, langs.SAMPLE_HZ


def short(name):
    name = name.split('(')[0]
    for prefix in ('facebook::react::', 'facebook::', 'std::__ndk1::', 'hermes::vm::'):
        name = name.replace(prefix, '')
    return name[:110]


def main():
    scenario, variant = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    lib = ReportLib()
    lib.SetRecordFile(os.path.join(OUT, f'prof-{scenario}-{variant}.data'))
    lib.SetSymfs(os.path.join(OUT, f'binary_cache-{variant}'))
    lib.ShowArtFrames(True)
    self_by_group, libc_callers, libc_leaf = {}, {}, {}
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        sym = lib.GetSymbolOfCurrentSample()
        g = langs.group_of(sym.dso_name, sym.symbol_name)
        key = f'{short(sym.symbol_name)}  [{os.path.basename(sym.dso_name)}]'
        self_by_group.setdefault(g, {}).setdefault(key, 0)
        self_by_group[g][key] += 1
        if g == 'libc':
            libc_leaf[short(sym.symbol_name)] = libc_leaf.get(short(sym.symbol_name), 0) + 1
            chain = lib.GetCallChainOfCurrentSample()
            caller = '(no caller)'
            for i in range(chain.nr):
                e = chain.entries[i].symbol
                if langs.group_of(e.dso_name, e.symbol_name) != 'libc' and 'operator new' not in e.symbol_name \
                        and 'operator delete' not in e.symbol_name and not e.symbol_name.startswith(('je_', 'malloc', 'free')):
                    caller = f'{short(e.symbol_name)}  [{os.path.basename(e.dso_name)}]'
                    break
            libc_callers[caller] = libc_callers.get(caller, 0) + 1
    lib.Close()
    ms = lambda n: n * 1000.0 / SAMPLE_HZ
    for g in langs.GROUPS:
        rows = self_by_group.get(g)
        if not rows:
            continue
        total = sum(rows.values())
        print(f'\n## {g}: {ms(total):.0f} ms')
        for k, n in sorted(rows.items(), key=lambda kv: -kv[1])[:top]:
            print(f'{ms(n):7.0f}  {k}')
    if libc_leaf:
        print('\n## libc by function')
        for k, n in sorted(libc_leaf.items(), key=lambda kv: -kv[1])[:8]:
            print(f'{ms(n):7.0f}  {k}')
        print('\n## libc time charged to its first non-libc caller')
        for k, n in sorted(libc_callers.items(), key=lambda kv: -kv[1])[:top]:
            print(f'{ms(n):7.0f}  {k}')


if __name__ == '__main__':
    main()
