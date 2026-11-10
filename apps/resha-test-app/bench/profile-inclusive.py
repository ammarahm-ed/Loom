#!/usr/bin/env python3
"""Inclusive CPU time per function for one thread of a bench/profile.sh recording.

    python3 bench/profile-inclusive.py <scenario> <variant> <main|worker|js|render> [pattern] [top]

Each sample counts once for every distinct function on its stack, so a caller's time includes its
callees. `pattern` (regex) limits which functions are listed, e.g. 'facebook::react|com\\.facebook'.
"""
import os
import re
import sys

NDK = sorted(os.path.join(os.path.expanduser('~/Library/Android/sdk/ndk'), d)
             for d in os.listdir(os.path.expanduser('~/Library/Android/sdk/ndk')))[-1]
sys.path.insert(0, os.path.join(NDK, 'simpleperf'))
from simpleperf_report_lib import ReportLib  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')
SAMPLE_HZ = 4000


def thread_of(sample):
    if sample.tid == sample.pid:
        return 'main'
    name = sample.thread_comm
    if name.startswith('mqt_v_js') or name.startswith('mqt_js'):
        return 'js'
    if name == 'RenderThread':
        return 'render'
    if 'bench.' in name:
        return 'worker'
    return 'other'


def main():
    scenario, variant, thread = sys.argv[1:4]
    pattern = re.compile(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4] else None
    top = int(sys.argv[5]) if len(sys.argv) > 5 else 40
    lib = ReportLib()
    lib.SetRecordFile(os.path.join(OUT, f'prof-{scenario}-{variant}.data'))
    lib.SetSymfs(os.path.join(OUT, f'binary_cache-{variant}'))
    lib.ShowArtFrames(True)
    counts, total = {}, 0
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        if thread_of(sample) != thread:
            continue
        total += 1
        chain = lib.GetCallChainOfCurrentSample()
        names = {lib.GetSymbolOfCurrentSample().symbol_name}
        names.update(chain.entries[i].symbol.symbol_name for i in range(chain.nr))
        for name in names:
            if pattern is None or pattern.search(name):
                counts[name] = counts.get(name, 0) + 1
    lib.Close()
    print(f'{thread}: {total * 1000 / SAMPLE_HZ:.0f} ms CPU')
    for name, n in sorted(counts.items(), key=lambda kv: -kv[1])[:top]:
        print(f'{n * 1000 / SAMPLE_HZ:8.0f} ms  {name[:150]}')


if __name__ == '__main__':
    main()
