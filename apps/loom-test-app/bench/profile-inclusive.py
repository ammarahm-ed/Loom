#!/usr/bin/env python3
"""Inclusive and self time per function, per thread, for a bench/profile.sh recording.

    python3 bench/profile-inclusive.py <scenario> <variant> [top] [filter]

`filter` keeps functions whose symbol contains it (default: Loom/RN C++ only).
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
    return name[:100]


def main():
    scenario, variant = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    flt = sys.argv[4] if len(sys.argv) > 4 else None
    lib = ReportLib()
    lib.SetRecordFile(os.path.join(OUT, f'prof-{scenario}-{variant}.data'))
    lib.SetSymfs(os.path.join(OUT, f'binary_cache-{variant}'))
    lib.ShowArtFrames(True)
    incl, selfc, totals = {}, {}, {}
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        thread = langs.thread_of(sample)
        totals[thread] = totals.get(thread, 0) + 1
        sym = lib.GetSymbolOfCurrentSample()
        chain = lib.GetCallChainOfCurrentSample()
        frames = [sym] + [chain.entries[i].symbol for i in range(chain.nr)]
        seen = set()
        for depth, f in enumerate(frames):
            g = langs.group_of(f.dso_name, f.symbol_name)
            if flt:
                if flt not in f.symbol_name:
                    continue
            elif g != 'C++ (RN + Loom)':
                continue
            k = short(f.symbol_name)
            if k in seen:
                continue
            seen.add(k)
            incl.setdefault(thread, {}).setdefault(k, 0)
            incl[thread][k] += 1
            if depth == 0:
                selfc.setdefault(thread, {}).setdefault(k, 0)
                selfc[thread][k] += 1
    lib.Close()
    ms = lambda n: n * 1000.0 / SAMPLE_HZ
    for thread in sorted(incl, key=lambda t: -totals.get(t, 0)):
        print(f'\n## {thread}: busy {ms(totals[thread]):.0f} ms')
        print('  inclusive:')
        for k, n in sorted(incl[thread].items(), key=lambda kv: -kv[1])[:top]:
            print(f'{ms(n):7.0f}  {k}')
        if thread in selfc:
            print('  self:')
            for k, n in sorted(selfc[thread].items(), key=lambda kv: -kv[1])[:top // 2]:
                print(f'{ms(n):7.0f}  {k}')


if __name__ == '__main__':
    main()
