#!/usr/bin/env python3
"""Where the CPU time goes, by language/runtime, for a bench/profile.sh recording.

    python3 bench/profile-languages.py <scenario> <variant> [<variant> ...]

Each sample is charged to the code it was executing (its leaf frame), grouped by the library or
file that code lives in, then summed per thread (main, JS, worker, RenderThread, other):

  JS (Hermes)      libhermesvm / libhermes*: the interpreter, GC and runtime running your JavaScript
  C++ (RN + Loom)  libreactnative, libappmodules, libhermestooling, libjsi, libyoga, libfbjni, libc++
  Java             compiled or interpreted Java/Kotlin: .oat/.odex/.vdex/.dex/.apk/.jar frames
  ART runtime      libart: interpreter, JNI transitions, GC, class loading
  Graphics         libhwui, Skia, GL/Vulkan and GPU drivers, gralloc
  libc             libc / libm / linker
  kernel           [kernel.kallsyms] and other kernel frames
  other            everything else (other system libraries)
"""
import os
import sys

NDK = sorted(os.path.join(os.path.expanduser('~/Library/Android/sdk/ndk'), d)
             for d in os.listdir(os.path.expanduser('~/Library/Android/sdk/ndk')))[-1]
sys.path.insert(0, os.path.join(NDK, 'simpleperf'))
from simpleperf_report_lib import ReportLib  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '.local', 'bench')
SAMPLE_HZ = 4000
GROUPS = ['JS (Hermes)', 'C++ (RN + Loom)', 'Java', 'ART runtime', 'Graphics', 'libc', 'kernel', 'other']
THREADS = ['main', 'JS thread', 'worker', 'RenderThread', 'other threads']


def group_of(dso, symbol):
    d = os.path.basename(dso)
    if 'jit' in dso.lower():
        return 'Java'
    if 'kernel' in dso or dso.startswith('[') and 'vdso' not in dso:
        return 'kernel'
    if d.startswith('libhermesvm') or (d.startswith('libhermes') and 'tooling' not in d):
        return 'JS (Hermes)'
    if d.endswith(('.oat', '.odex', '.vdex', '.dex', '.apk', '.jar')) or dso.endswith(']') and 'jit' in dso.lower():
        return 'Java'
    if d in ('libart.so', 'libartbase.so', 'libart-compiler.so', 'libdexfile.so', 'libnativebridge.so'):
        return 'ART runtime'
    if d.startswith(('libreactnative', 'libappmodules', 'libhermestooling', 'libjsi', 'libyoga',
                     'libfbjni', 'libc++_shared', 'libreact', 'libjscexecutor', 'libfolly')):
        return 'C++ (RN + Loom)'
    if d.startswith(('libhwui', 'libskia', 'libGLES', 'libEGL', 'libvulkan', 'libgui', 'libui',
                     'libgralloc', 'libmali', 'libGLESv', 'libpvr', 'libsrv', 'libIMG', 'libsurfaceflinger')) \
            or 'gpu' in dso.lower() or '/egl/' in dso or 'hw/gralloc' in dso:
        return 'Graphics'
    if d in ('libc.so', 'libm.so', 'libdl.so', 'linker', 'linker64', 'ld-android.so'):
        return 'libc'
    return 'other'


def thread_of(sample):
    name = sample.thread_comm
    if sample.tid == sample.pid:
        return 'main'
    if name.startswith('mqt_v_js') or name.startswith('mqt_js'):
        return 'JS thread'
    if name == 'RenderThread':
        return 'RenderThread'
    if 'bench.' in name or name.startswith('Loom') or name.startswith('loom'):
        return 'worker'
    return 'other threads'


def analyze(scenario, variant):
    lib = ReportLib()
    lib.SetRecordFile(os.path.join(OUT, f'prof-{scenario}-{variant}.data'))
    lib.SetSymfs(os.path.join(OUT, f'binary_cache-{variant}'))
    lib.ShowArtFrames(True)
    table = {}
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        sym = lib.GetSymbolOfCurrentSample()
        t = thread_of(sample)
        g = group_of(sym.dso_name, sym.symbol_name)
        table.setdefault(t, {}).setdefault(g, 0)
        table[t][g] += 1
    lib.Close()
    return table


def ms(n):
    return n * 1000.0 / SAMPLE_HZ


def main():
    scenario, variants = sys.argv[1], sys.argv[2:]
    results = {v: analyze(scenario, v) for v in variants}
    print(f'## {scenario}: CPU ms by language/runtime (all threads)\n')
    print('| | ' + ' | '.join(variants) + ' |')
    print('|---|' + '---|' * len(variants))
    for g in GROUPS:
        vals = [sum(results[v].get(t, {}).get(g, 0) for t in THREADS) for v in variants]
        if any(vals):
            totals = [sum(sum(x.values()) for x in results[v].values()) for v in variants]
            cells = [f'{ms(x):.0f} ({100 * x / max(tot, 1):.0f}%)' for x, tot in zip(vals, totals)]
            print(f'| {g} | ' + ' | '.join(cells) + ' |')
    totals = [sum(sum(x.values()) for x in results[v].values()) for v in variants]
    print('| **total** | ' + ' | '.join(f'**{ms(t):.0f}**' for t in totals) + ' |\n')
    for v in variants:
        print(f'### {v}: per thread (ms)\n')
        print('| thread | total | ' + ' | '.join(GROUPS) + ' |')
        print('|---|---|' + '---|' * len(GROUPS))
        for t in THREADS:
            row = results[v].get(t, {})
            if not row:
                continue
            print(f'| {t} | {ms(sum(row.values())):.0f} | ' + ' | '.join(f'{ms(row.get(g, 0)):.0f}' for g in GROUPS) + ' |')
        print()


if __name__ == '__main__':
    main()
