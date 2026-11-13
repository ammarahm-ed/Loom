#!/usr/bin/env python3
"""Builds the low-end CPU breakdown report from bench/profile.sh recordings and Hermes JS profiles.

    python3 bench/lowend-breakdown.py > <repo>/.local/bench/lowend-breakdown.md

Inputs in <repo>/.local/bench: prof-<scenario>-<variant>.data (+ binary_cache-<variant>) and
jsprof/js-<scenario>-<variant>.json. One recording per scenario and configuration.
"""
import json
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from importlib import import_module  # noqa: E402

langs = import_module('profile-languages')
OUT = langs.OUT
VARIANTS = ['stock', 'on', 'js']
NAMES = {'stock': 'Stock', 'on': 'Loom, JS on main', 'js': 'Loom, JS own thread'}
SCENARIOS = [
    ('startup', 'Startup (first 3 s of a cold launch)'),
    ('burst-raf', '300-cell re-render burst (6 s)'),
    ('latency-heavy', 'Taps that re-render 300 cells (6 s)'),
    ('scroll', 'Scrolling a 2000-row list (6 s)'),
    ('anim', '40 native-driver animations (6 s)'),
    ('js-anim', '40 JS-driven animations (6 s)'),
]

JS_BUCKETS = [
    ('Host calls into the renderer', r'^\[HostFunction\]'),
    ('GC', r'^\[GC'),
    ('RN prop diffing / style', r'^(diff|diffProperties|diffNestedProperty|addProperties|addNestedProperty|'
                                r'clearProperties|clearNestedProperty|create|createAttributePayload|'
                                r'flattenStyle|processColor|normalizeColor|deepDiffer|fastAddProperties|'
                                r'defaultDiffer|resolveObject|restoreDeletedValuesInNestedArray)$'),
    ('React commit', r'^(commit|recursivelyTraverse|flush|completeRoot|replaceContainerChildren|'
                     r'finalizeContainerChildren|createContainerChildSet|appendAllChildren)'),
    ('React reconciler', r'^(beginWork|completeWork|completeUnitOfWork|performUnitOfWork|workLoop|'
                         r'renderRoot|performWorkOnRoot|reconcile|createWorkInProgress|createFiber|'
                         r'bubbleProperties|updateHostComponent|update|mountIndeterminate|renderWithHooks|'
                         r'cloneChildFibers|createChild|placeSingleChild|useFiber|scheduleUpdate|'
                         r'markUpdate|dispatch|enqueue|ensureRootIsScheduled|processUpdateQueue|'
                         r'bailout|attemptEarly|prepareToReadContext|readContext|mountWorkInProgressHook|'
                         r'updateWorkInProgressHook|use[A-Z]|mount[A-Z]|getRootForUpdatedFiber|'
                         r'shouldYield|unstable_|scheduler|requestUpdateLane|resetHooks)'),
    ('Native built-ins', r'^\[Native\]'),
    ('Component render (RN components + app)', r'.*'),
]


def js_profile(scenario, variant):
    path = os.path.join(OUT, 'jsprof', f'js-{scenario}-{variant}.json')
    if not os.path.exists(path):
        return None
    data = json.load(open(path))
    frames, samples = data.get('stackFrames', {}), data.get('samples', [])
    busy, buckets, selfc = 0, Counter(), Counter()
    for s in samples:
        f = frames.get(str(s.get('sf')), {})
        name = (f.get('name') or '?').split('(')[0].strip()
        if name in ('[root]', '') or f.get('category') == 'Metadata':
            continue
        busy += 1
        selfc[name] += 1
        for label, pat in JS_BUCKETS:
            if re.match(pat, name):
                buckets[label] += 1
                break
    return busy, buckets, selfc


def pct(n, d):
    return f'{100 * n / d:.0f}%' if d else '-'


def main():
    print('# Low-end Android CPU breakdown: Infinix X6516\n')
    print('Unisoc SC9863A (8x Cortex-A55), 1.9 GB RAM, 32-bit (armeabi-v7a), Android 12, 60 Hz. Release')
    print('builds installed like a store install. One recording per scenario and configuration:')
    print('`simpleperf` at 4 kHz for native/Java time, the Hermes sampling profiler for JavaScript.\n')
    print('Configurations: **Stock** = React Native without Loom; **Loom, JS on main** = '
          '`loom.js=main`; **Loom, JS own thread** = `loom.js=background` (both with the worker).\n')
    for scenario, title in SCENARIOS:
        print(f'## {title}\n')
        res = {}
        for v in VARIANTS:
            p = os.path.join(OUT, f'prof-{scenario}-{v}.data')
            res[v] = langs.analyze(scenario, v) if os.path.exists(p) else None
        print('### CPU time by language/runtime (ms, all threads)\n')
        print('| | ' + ' | '.join(NAMES[v] for v in VARIANTS) + ' |')
        print('|---|' + '---|' * len(VARIANTS))
        totals = {v: sum(sum(x.values()) for x in res[v].values()) if res[v] else 0 for v in VARIANTS}
        for g in langs.GROUPS:
            row = []
            for v in VARIANTS:
                if not res[v]:
                    row.append('-'); continue
                n = sum(res[v].get(t, {}).get(g, 0) for t in langs.THREADS)
                row.append(f'{langs.ms(n):.0f} ({pct(n, totals[v])})')
            print(f'| {g} | ' + ' | '.join(row) + ' |')
        print('| **total** | ' + ' | '.join(f'**{langs.ms(totals[v]):.0f}**' for v in VARIANTS) + ' |\n')
        print('### Busy time per thread (ms)\n')
        print('| thread | ' + ' | '.join(NAMES[v] for v in VARIANTS) + ' |')
        print('|---|' + '---|' * len(VARIANTS))
        for t in langs.THREADS:
            vals = [langs.ms(sum(res[v].get(t, {}).values())) if res[v] else 0 for v in VARIANTS]
            if any(vals):
                print(f'| {t} | ' + ' | '.join(f'{x:.0f}' for x in vals) + ' |')
        print()
        js = {v: js_profile(scenario, v) for v in VARIANTS}
        if any(js.values()):
            print('### JavaScript time by category (share of JS-busy samples)\n')
            print('| | ' + ' | '.join(NAMES[v] for v in VARIANTS) + ' |')
            print('|---|' + '---|' * len(VARIANTS))
            for label, _ in JS_BUCKETS:
                print(f'| {label} | ' + ' | '.join(pct(js[v][1][label], js[v][0]) if js[v] else '-' for v in VARIANTS) + ' |')
            print('| JS-busy samples | ' + ' | '.join(str(js[v][0]) if js[v] else '-' for v in VARIANTS) + ' |\n')
            print('Top JS functions (self time):\n')
            for v in VARIANTS:
                if not js[v]:
                    continue
                busy, _, selfc = js[v]
                top = ', '.join(f'{n} {pct(k, busy)}' for n, k in selfc.most_common(8))
                print(f'- **{NAMES[v]}**: {top}')
            print()


if __name__ == '__main__':
    main()
