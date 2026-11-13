#!/usr/bin/env python3
"""Summarizes a Hermes sampling-profiler trace (BenchProbe `--ei jsprofile <ms>`).

    python3 bench/jsprofile-report.py <jsprofile.json> [top]

Prints, per JavaScript function: self samples (time spent in the function itself) and total
samples (including callees), plus how the samples split between JavaScript, native host
functions called from JS (e.g. the renderer's createNode/cloneNode, JSI modules) and GC.
"""
import json
import sys
from collections import Counter


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    data = json.load(open(path))
    frames = data.get('stackFrames', {})
    samples = data.get('samples', [])
    if not samples:
        print('no samples'); return

    def label(fid):
        f = frames.get(str(fid), {})
        name = f.get('name', '?')
        name = name.split('(')[0].strip() or '(anonymous)'
        cat = f.get('category', '')
        line = f.get('line')
        return f'{name}' + (f' :{line}' if line else ''), cat

    self_c, total_c, cat_c = Counter(), Counter(), Counter()
    for s in samples:
        fid = s.get('sf')
        if fid is None:
            continue
        name, cat = label(fid)
        self_c[name] += 1
        cat_c[cat or 'unknown'] += 1
        seen = set()
        cur = str(fid)
        while cur and cur in frames:
            n, _ = label(cur)
            if n not in seen:
                total_c[n] += 1
                seen.add(n)
            cur = str(frames[cur].get('parent')) if 'parent' in frames[cur] else None
    n = len(samples)
    print(f'{n} samples\n')
    print('## by category (leaf frame)')
    for c, k in cat_c.most_common():
        print(f'{100 * k / n:6.1f}%  {c}')
    print('\n## self time (top)')
    for name, k in self_c.most_common(top):
        print(f'{100 * k / n:6.1f}%  {name}')
    print('\n## total time incl. callees (top)')
    for name, k in total_c.most_common(top):
        print(f'{100 * k / n:6.1f}%  {name}')


if __name__ == '__main__':
    main()
