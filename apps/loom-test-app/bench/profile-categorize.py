#!/usr/bin/env python3
"""Buckets a bench/profile.sh recording into React Native subsystems.

    python3 bench/profile-categorize.py <scenario> <variant> [<variant> ...]

Each sample is charged to the first subsystem found walking its call chain from the leaf towards
the root, so malloc or JNI work done *for* Yoga counts as layout, while JNI/allocator time with no
recognizable caller stays in its own bucket. Times are CPU milliseconds (samples / sample rate),
split into the threads that matter: main (UI, and JS too on the LOOM_JS_ON_MAIN builds), the JS thread
(mqt_v_js / mqt_js), the Loom worker, RenderThread, and everything else.
"""
import os
import re
import sys

NDK = sorted(
    os.path.join(os.path.expanduser('~/Library/Android/sdk/ndk'), d)
    for d in os.listdir(os.path.expanduser('~/Library/Android/sdk/ndk'))
)[-1]
sys.path.insert(0, os.path.join(NDK, 'simpleperf'))
from simpleperf_report_lib import ReportLib  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '.local', 'bench')
SAMPLE_HZ = 4000

# Ordered: the first match while walking leaf -> root wins, so specific subsystems go first.
CATEGORIES = [
    ('gc', r'art::gc::|HeapTaskDaemon|hermes::vm::(HadesGC|GCBase|.*Collect)'),
    ('text', r'TextLayoutManager|ParagraphShadowNode|PreparedLayout|android\.text\.|minikin|'
             r'StaticLayout|TextView|ReactTextView|TextAttributes|AttributedString|measureText|'
             r'Paragraph|Spannable|android\.graphics\.Paint\.'),
    ('layout', r'yoga::|YG[A-Z]|LayoutableShadowNode|layoutTree|calculateLayout'),
    ('folly::dynamic', r'folly::dynamic|folly::.*parseJson|folly::toJson|SpookyHash|'
                       r'jsi::dynamicFromValue|valueFromDynamic|convertDynamic'),
    ('props parse', r'RawProps|ViewProps::|BaseViewProps|PropsParser|convertRawProp|fromRawValue|'
                    r'Props::Props|HostPlatformViewProps'),
    ('shadow tree / diff', r'ShadowNode|ShadowTree|Differentiator|calculateShadowViewMutations|'
                           r'MountingCoordinator|sliceChildShadowNodeViewPairs|ShadowView|'
                           r'StubViewTree|updateMountedFlag|TelemetryController'),
    ('loom renderer', r'LoomRenderer|LoomDirect|DraftNode|LoomEventManager'),
    ('mount (java)', r'com\.facebook\.react\.(fabric|uimanager|views)\.|SurfaceMountingManager|'
                     r'MountItem|FabricMountingManager|ViewManager|android\.view\.ViewGroup\.(add|remove)'),
    ('jni', r'art::JNI|art::.*JniMethod|facebook::jni::|JNIEnv|art_jni_|DecodeJObject|'
            r'IndirectReferenceTable|ReadableNativeMap|WritableNativeMap|NativeMap|NativeArray'),
    ('js (hermes)', r'hermes::|libhermesvm|facebook::hermes|HermesRuntime|jsi::'),
    ('draw (hwui)', r'libhwui|uirenderer|skia|Sk[A-Z]|GrGL|libGLES|adreno|android\.view\.View\.draw|'
                    r'RenderNode|Choreographer|ThreadedRenderer'),
    ('malloc', r'scudo|malloc|operator new|operator delete|je_|free$'),
    ('react other', r'facebook::react::|com\.facebook\.react\.'),
]
CATEGORIES = [(name, re.compile(pattern)) for name, pattern in CATEGORIES]


def thread_bucket(sample):
    name = sample.thread_comm
    if sample.tid == sample.pid:
        return 'main'
    if name.startswith('mqt_v_js') or name.startswith('mqt_js'):
        return 'js thread'
    if name == 'RenderThread':
        return 'RenderThread'
    # An unnamed native thread (the Loom worker is a plain std::thread) keeps the process name,
    # which the kernel truncates to its last 15 characters, e.g. "estapp.bench.on".
    if name.endswith('.bench.on') or name.endswith('.bench.off') or 'bench.' in name:
        return 'worker'
    return 'other'


def categorize(sample_symbol, callchain):
    frames = [sample_symbol] + list(callchain)
    for frame in frames:
        text = f'{frame.symbol_name} {frame.dso_name}'
        for name, pattern in CATEGORIES:
            if pattern.search(text):
                return name
    return 'uncategorized'


def analyze(scenario, variant):
    lib = ReportLib()
    lib.SetRecordFile(os.path.join(OUT, f'prof-{scenario}-{variant}.data'))
    lib.SetSymfs(os.path.join(OUT, f'binary_cache-{variant}'))
    lib.ShowArtFrames(True)
    totals = {}
    threads = {}
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        symbol = lib.GetSymbolOfCurrentSample()
        chain = lib.GetCallChainOfCurrentSample()
        frames = [chain.entries[i].symbol for i in range(chain.nr)]
        thread = thread_bucket(sample)
        category = categorize(symbol, frames)
        threads.setdefault(thread, {}).setdefault(category, 0)
        threads[thread][category] += 1
        totals[category] = totals.get(category, 0) + 1
    lib.Close()
    return totals, threads


def ms(samples):
    return samples * 1000.0 / SAMPLE_HZ


def main():
    scenario, variants = sys.argv[1], sys.argv[2:]
    results = {v: analyze(scenario, v) for v in variants}
    categories = [name for name, _ in CATEGORIES] + ['uncategorized']
    print(f'## {scenario}: CPU ms by subsystem, all app threads (6 s recording unless startup)\n')
    print('| subsystem | ' + ' | '.join(variants) + ' |')
    print('|---|' + '---|' * len(variants))
    for category in categories:
        values = [results[v][0].get(category, 0) for v in variants]
        if any(values):
            print(f'| {category} | ' + ' | '.join(f'{ms(x):.0f}' for x in values) + ' |')
    print('| **total** | ' + ' | '.join(f'**{ms(sum(results[v][0].values())):.0f}**' for v in variants) + ' |')
    print()
    print('| thread | ' + ' | '.join(variants) + ' |')
    print('|---|' + '---|' * len(variants))
    for thread in ['main', 'js thread', 'worker', 'RenderThread', 'other']:
        values = [sum(results[v][1].get(thread, {}).values()) for v in variants]
        print(f'| {thread} | ' + ' | '.join(f'{ms(x):.0f}' for x in values) + ' |')
    print()
    for v in variants:
        for thread in ['main', 'js thread', 'worker']:
            cats = results[v][1].get(thread)
            if not cats:
                continue
            top = sorted(cats.items(), key=lambda kv: -kv[1])[:6]
            print(f'- {v} / {thread}: ' + ', '.join(f'{k} {ms(n):.0f}' for k, n in top))


if __name__ == '__main__':
    main()
