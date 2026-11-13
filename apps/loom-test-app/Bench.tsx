/**
 * Benchmark scenarios, driven from the host by bench/run.js.
 *
 * `adb shell am start -n <app>/com.loomtestapp.MainActivity --es scenario <name>` renders one of
 * these instead of the smoke test. Each one is the same JS on every build, so the only variable
 * between stock React Native, Loom all-off and Loom all-on is the native side.
 *
 * Timing that depends on pixels (first content, tap to visible change) is measured natively by
 * BenchProbe.kt. What is measured here is JS-observable throughput, reported as one
 * `[bench] result {...}` line that run.js parses out of logcat.
 *
 * Scenario names ending in `-busy` add a JS load: 20 ms of synchronous work every 50 ms, roughly
 * what a real app's business logic, JSON parsing and re-renders cost while the user is interacting.
 *
 * @format
 */

import React, {
  startTransition,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
} from 'react';
// @ts-ignore: plain JS bench helper
import { fabricStatsSummary, resetFabricStats } from './benchFabricProbe';
import {
  Animated,
  Easing,
  FlatList,
  NativeModules,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';

const BUSY_MS = 20;
const BUSY_PERIOD_MS = 50;

function report(scenario: string, data: Record<string, number | string>) {
  const line = `[bench] result ${JSON.stringify({ scenario, ...data })}`;
  console.log(line);
  // iOS Release builds do not print console.log to the device console; BenchProbe.mm forwards it
  // (and closes the frame-stats window). Absent on Android.
  NativeModules.LoomBuildConfig?.log?.(line);
}

function useBusyLoad(enabled: boolean) {
  useEffect(() => {
    if (!enabled) {
      return;
    }
    const id = setInterval(() => {
      const end = Date.now() + BUSY_MS;
      while (Date.now() < end) {
        // Burn the JS thread, like a heavy reducer or JSON.parse would.
      }
    }, BUSY_PERIOD_MS);
    return () => clearInterval(id);
  }, [enabled]);
}

const PALETTE = ['#1f6feb', '#1a7f37', '#9a6700', '#cf222e', '#8250df'];

// --- startup: a typical first screen (header, a few cards, a list), 40 rows ---

function FeedRow({ index }: { index: number }) {
  return (
    <View style={styles.feedRow}>
      <View
        style={[
          styles.avatar,
          { backgroundColor: PALETTE[index % PALETTE.length] },
        ]}
      />
      <View style={styles.feedBody}>
        <Text style={styles.feedTitle}>Item {index + 1}</Text>
        <Text style={styles.feedText} numberOfLines={2}>
          A line of ordinary body text, long enough to wrap onto a second line
          on a phone-sized screen.
        </Text>
      </View>
    </View>
  );
}

function Startup() {
  // TTI: the first screen's effect, as react-native-performance's `screenInteractive` mark.
  useEffect(() => {
    NativeModules.LoomBuildConfig?.mark?.('screenInteractive');
  }, []);
  return (
    <View nativeID="bench-ready" style={styles.container}>
      <Text style={styles.header}>Home</Text>
      {Array.from({ length: 40 }, (_, index) => (
        <FeedRow key={index} index={index} />
      ))}
    </View>
  );
}

// --- scroll: a long text-heavy FlatList, scrolled by `input swipe` from the host ---

const SCROLL_DATA = Array.from({ length: 2000 }, (_, index) => index);

// iOS has no `input swipe`: with the `-autoscroll` launch argument the scroll is driven from JS instead
// (without it the list scrolls by finger), 12 flicks of 1400 px over
// 120 ms, 500 ms apart, starting 1.5 s after mount (the same cadence run.js uses on Android).
// Every offset is applied with scrollToOffset from a requestAnimationFrame loop.
function useAutoScroll(
  list: React.RefObject<FlatList<number> | null>,
  enabled: boolean,
) {
  useEffect(() => {
    if (Platform.OS !== 'ios' || !enabled) {
      return;
    }
    let raf = 0;
    let stopped = false;
    const startTimer = setTimeout(() => {
      const t0 = performance.now();
      const step = () => {
        if (stopped) {
          return;
        }
        const elapsed = performance.now() - t0;
        const flick = Math.floor(elapsed / 500);
        if (flick >= 12) {
          return;
        }
        const within = Math.min(1, (elapsed - flick * 500) / 120);
        list.current?.scrollToOffset({
          offset: (flick + within) * 1400,
          animated: false,
        });
        raf = requestAnimationFrame(step);
      };
      raf = requestAnimationFrame(step);
    }, 1500);
    return () => {
      stopped = true;
      clearTimeout(startTimer);
      cancelAnimationFrame(raf);
    };
  }, [list, enabled]);
}

// scroll-blank: the share of the viewport that FlatList's fill-rate sampler saw unrendered while
// scrolling (VirtualizedList placeholders). Reported once, 11 s after mount (the scroll gestures end
// at about 9.5 s). blankPct is pixels_blank / pixels_sampled; a lower number means less blanking.
function useBlankness(enabled: boolean, list: React.RefObject<any>) {
  // The sample rate is read when a list is constructed, so it is set during render, before the
  // list below is created, not in an effect.
  useState(() => {
    if (enabled) {
      // eslint-disable-next-line @typescript-eslint/no-var-requires
      const { FillRateHelper } = require('@react-native/virtualized-lists').default;
      FillRateHelper.setSampleRate(1);
      FillRateHelper.setMinSampleCount(1);
    }
    return null;
  });
  useEffect(() => {
    if (!enabled) {
      return;
    }
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const { FillRateHelper } = require('@react-native/virtualized-lists').default;
    let blank = 0;
    let sampled = 0;
    let anyBlankMs = 0;
    let mostlyBlankMs = 0;
    const sub = FillRateHelper.addListener((info: any) => {
      blank += info.pixels_blank;
      sampled += info.pixels_sampled;
      anyBlankMs += info.any_blank_ms;
      mostlyBlankMs += info.mostly_blank_ms;
    });
    const timer = setTimeout(() => {
      // The sampler only flushes when the list unmounts; flush it by hand.
      list.current?._listRef?._fillRateHelper?.deactivateAndFlush();
      report('scroll-blank', {
        blankPct: sampled > 0 ? +((100 * blank) / sampled).toFixed(2) : -1,
        anyBlankMs: Math.round(anyBlankMs),
        mostlyBlankMs: Math.round(mostlyBlankMs),
        sampledPx: Math.round(sampled),
      });
    }, 11000);
    return () => {
      clearTimeout(timer);
      sub.remove();
    };
  }, [enabled, list]);
}

// Raw cost of the list's window updates: every _updateCellsToRender that changes state is timed from
// the call to its commit callback (JS render + commit, host calls included; the native apply
// follows on the UI thread and shows in mainCpuMs), and rows are counted per renderItem call.
// Reported once at 11 s as scroll-stats.
const scrollStats = { rows: 0, updates: 0, updateMs: 0, maxUpdateMs: 0 };

function useWindowUpdateStats(list: React.RefObject<any>) {
  useEffect(() => {
    const target = list.current?._listRef;
    if (!target || typeof target._updateCellsToRender !== 'function') {
      return;
    }
    const original = target._updateCellsToRender;
    target._updateCellsToRender = (cb?: () => void) => {
      const t0 = performance.now();
      let returned = false;
      original(() => {
        if (returned) {
          const ms = performance.now() - t0;
          scrollStats.updates++;
          scrollStats.updateMs += ms;
          scrollStats.maxUpdateMs = Math.max(scrollStats.maxUpdateMs, ms);
        }
        cb?.();
      });
      returned = true;
    };
    const timer = setTimeout(() => {
      report('scroll-stats', {
        rows: scrollStats.rows,
        updates: scrollStats.updates,
        avgUpdateMs: scrollStats.updates ? +(scrollStats.updateMs / scrollStats.updates).toFixed(2) : 0,
        maxUpdateMs: +scrollStats.maxUpdateMs.toFixed(1),
        totalUpdateMs: Math.round(scrollStats.updateMs),
      });
    }, 11000);
    return () => {
      clearTimeout(timer);
      target._updateCellsToRender = original;
    };
  }, [list]);
}

function Scroll({
  busy,
  autoscroll,
  blank = false,
}: {
  busy: boolean;
  autoscroll: boolean;
  blank?: boolean;
}) {
  useBusyLoad(busy);
  const list = useRef<FlatList<number>>(null);
  useBlankness(blank, list);
  useWindowUpdateStats(list);
  useAutoScroll(list, autoscroll);
  return (
    <View nativeID="bench-ready" style={styles.container}>
      <FlatList
        ref={list}
        data={SCROLL_DATA}
        keyExtractor={index => String(index)}
        renderItem={({ item }) => {
          scrollStats.rows++;
          return <FeedRow index={item} />;
        }}
      />
    </View>
  );
}

// --- anim: 40 looping boxes, native driver or JS driver ---

function AnimBox({ index, native }: { index: number; native: boolean }) {
  const value = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    // A single timing loop, not loop(sequence(...)): Animated cannot run a looped sequence on the
    // native driver and silently restarts it from JS every iteration. The out-and-back motion
    // comes from the triangular interpolation below instead.
    const loop = Animated.loop(
      Animated.timing(value, {
        toValue: 1,
        duration: 1400 + (index % 5) * 120,
        easing: Easing.linear,
        useNativeDriver: native,
      }),
    );
    loop.start();
    return () => loop.stop();
  }, [index, native, value]);
  return (
    <Animated.View
      style={[
        styles.animBox,
        {
          backgroundColor: PALETTE[index % PALETTE.length],
          opacity: value.interpolate({
            inputRange: [0, 0.5, 1],
            outputRange: [0.4, 1, 0.4],
          }),
          transform: [
            {
              translateX: value.interpolate({
                inputRange: [0, 0.5, 1],
                outputRange: [0, 260, 0],
              }),
            },
          ],
        },
      ]}
    />
  );
}

function Anim({ native, busy }: { native: boolean; busy: boolean }) {
  useBusyLoad(busy);
  return (
    <View nativeID="bench-ready" style={styles.container}>
      {Array.from({ length: 40 }, (_, index) => (
        <AnimBox key={index} index={index} native={native} />
      ))}
    </View>
  );
}

// --- burst: back-to-back React commits on a 300-cell grid ---

const BURST_CELLS = 300;

const SmallCell = React.memo(function SmallCell({
  index,
  value,
}: {
  index: number;
  value: number;
}) {
  return (
    <View
      style={[
        styles.cell,
        { backgroundColor: PALETTE[(index + value) % PALETTE.length] },
      ]}>
      <Text style={styles.cellText}>{(index * 7 + value) % 100}</Text>
    </View>
  );
});
const BURST_COMMITS = 300;

// burst-resize-once: each commit resizes a tenth of the labels, a different tenth every commit, so
// a label is resized once every 10 frames: one-off resizes (a label wrapping to a new width after a
// commit), not a label animating. The other cells receive identical props and bail out.
const OnceCell = React.memo(function OnceCell({
  index,
  width,
}: {
  index: number;
  width: number;
}) {
  return (
    <View
      style={[styles.cell, { backgroundColor: PALETTE[index % PALETTE.length] }]}>
      <Text style={[styles.cellText, { width, textAlign: 'center' }]}>
        {`${index}x`}
      </Text>
    </View>
  );
});

function Burst({
  mode,
  small = false,
  unique = false,
  resize = false,
  once = false,
}: {
  mode: 'raf' | 'timeout';
  small?: boolean;
  // burst-unique: every cell shows text no other cell shows (`step:index`, step mod 100 so it fits
  // the cell on one line and wrapping does not move it), so a measure cache rarely hits; the
  // repeated-text scenarios show ~100 strings.
  unique?: boolean;
  // burst-resize: the text stays the same and only its width changes, so every commit redraws
  // text at a new size (line breaks and alignment change with it).
  resize?: boolean;
  once?: boolean;
}) {
  const [step, setStep] = useState(0);
  const start = useRef(0);
  const smallValues = useRef<number[]>([]);

  useEffect(() => {
    // Let startup settle before measuring.
    const id = setTimeout(() => {
      start.current = performance.now();
      setStep(1);
    }, 1500);
    return () => clearTimeout(id);
  }, []);

  useEffect(() => {
    if (step === 0) {
      return;
    }
    if (step >= BURST_COMMITS) {
      const ms = performance.now() - start.current;
      report(`burst-${small ? 'small' : unique ? 'unique' : once ? 'resize-once' : resize ? 'resize' : mode}`, {
        commits: BURST_COMMITS,
        totalMs: Math.round(ms),
        perCommitMs: Number((ms / BURST_COMMITS).toFixed(3)),
      });
      return;
    }
    const next = () => setStep(value => value + 1);
    if (mode === 'raf') {
      requestAnimationFrame(next);
    } else {
      // burst-resize-once commits from a timer, not a requestAnimationFrame callback: a commit
      // made by rAF work is an animation frame, whose text is drawn in that frame.
      setTimeout(next, once ? 16 : 0);
    }
  }, [mode, once, step]);

  // burst-small: only the cell the step lands on changes, as in most real updates (a counter, a
  // toggled row). The rest receive identical props and bail out, so React hands the renderer a
  // handful of changed nodes instead of the whole grid.
  if (small && step > 0) {
    smallValues.current[step % BURST_CELLS] = step;
  }
  if (once) {
    return (
      <View nativeID="bench-ready" style={[styles.container, styles.grid]}>
        {Array.from({ length: BURST_CELLS }, (_, index) => (
          <OnceCell
            key={index}
            index={index}
            width={14 + (Math.floor((step + 10 - (index % 10)) / 10) % 4) * 6}
          />
        ))}
      </View>
    );
  }
  if (small) {
    return (
      <View nativeID="bench-ready" style={[styles.container, styles.grid]}>
        {Array.from({ length: BURST_CELLS }, (_, index) => (
          <SmallCell
            key={index}
            index={index}
            value={smallValues.current[index] ?? 0}
          />
        ))}
      </View>
    );
  }
  return (
    <View nativeID="bench-ready" style={[styles.container, styles.grid]}>
      {Array.from({ length: BURST_CELLS }, (_, index) => (
        <View
          key={index}
          style={[
            styles.cell,
            { backgroundColor: PALETTE[(index + step) % PALETTE.length] },
          ]}>
          {resize ? (
            <Text
              style={[
                styles.cellText,
                { width: 14 + ((index + step) % 4) * 6, textAlign: 'center' },
              ]}>
              {`${index}x`}
            </Text>
          ) : (
            <Text style={styles.cellText}>
              {unique ? `${step % 100}:${index}` : (index * 7 + step) % 100}
            </Text>
          )}
        </View>
      ))}
    </View>
  );
}

// --- heavy: native-driven animation keeps running while a large update lands every 500 ms ---

const HEAVY_CELLS = 1000;
const HEAVY_UPDATES = 12;

// `transition` triggers the update inside startTransition, so React may time-slice it and the
// runtime may hand the main thread back to frames between slices. `sync` is the urgent case: a
// plain setState, which React renders in one piece. Reported: how long each update took to land
// (trigger -> committed in JS), alongside the frame stats run.js reads from gfxinfo.
function Heavy({ transition }: { transition: boolean }) {
  const [step, setStep] = useState(0);
  const triggered = useRef(0);
  const landings = useRef<number[]>([]);

  useEffect(() => {
    let count = 0;
    const id = setInterval(() => {
      count += 1;
      if (count > HEAVY_UPDATES) {
        clearInterval(id);
        const sorted = [...landings.current].sort((a, b) => a - b);
        report(transition ? 'transition-heavy' : 'heavy-sync', {
          updates: sorted.length,
          landMs: Number(
            (sorted.reduce((a, b) => a + b, 0) / (sorted.length || 1)).toFixed(1),
          ),
          landP90Ms: Number((sorted[Math.floor(sorted.length * 0.9)] ?? 0).toFixed(1)),
        });
        return;
      }
      triggered.current = performance.now();
      if (transition) {
        startTransition(() => setStep(count));
      } else {
        setStep(count);
      }
    }, 500);
    return () => clearInterval(id);
  }, [transition]);

  useLayoutEffect(() => {
    if (step > 0) {
      landings.current.push(performance.now() - triggered.current);
    }
  }, [step]);

  return (
    <View nativeID="bench-ready" style={styles.container}>
      {Array.from({ length: 12 }, (_, index) => (
        <AnimBox key={index} index={index} native />
      ))}
      <View style={styles.grid}>
        {Array.from({ length: HEAVY_CELLS }, (_, index) => (
          <View
            key={index}
            style={[
              styles.cell,
              { backgroundColor: PALETTE[(index + step) % PALETTE.length] },
            ]}>
            <Text style={styles.cellText}>{(index * 7 + step) % 100}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}

// --- latency: tap a tile, measure DOWN -> first frame showing the change (BenchProbe.kt) ---

// Timestamps for the latency breakdown. performance.now() reads the same CLOCK_MONOTONIC as
// SystemClock.uptimeMillis(), so run.js can line these up with BenchProbe's `down` and `probe`.
function mark(name: string) {
  console.log(`[bench] mark ${name} t=${performance.now().toFixed(2)}`);
}

function Latency({
  busy,
  heavy,
  snp,
}: {
  busy: boolean;
  heavy: boolean;
  snp: boolean;
}) {
  const [on, setOn] = useState(false);
  const snpOn = useRef(false);
  const probe = useRef<React.ComponentRef<typeof View>>(null);
  const first = useRef(true);
  const pendingTap = useRef(false);
  if (pendingTap.current) {
    // First line of the re-render the tap caused: event -> here is scheduling, here -> commit is
    // React rendering plus the native calls it makes.
    pendingTap.current = false;
    mark('render');
  }
  useBusyLoad(busy);
  useLayoutEffect(() => {
    if (first.current) {
      first.current = false;
      return;
    }
    mark('commit');
    console.log(`[bench] fabric ${JSON.stringify(fabricStatsSummary())}`);
  }, [on]);
  const onPressIn = () => {
    resetFabricStats();
    mark('event');
    if (snp) {
      // The floor: skip React entirely, like setNativeProps or a Reanimated worklet would.
      snpOn.current = !snpOn.current;
      probe.current?.setNativeProps({
        style: { opacity: snpOn.current ? 0.5 : 1 },
      });
      mark('commit');
    } else {
      pendingTap.current = true;
      setOn(value => !value);
    }
  };
  return (
    <View nativeID="bench-ready" style={styles.container}>
      <Pressable onPressIn={onPressIn}>
        <View
          ref={probe}
          nativeID="latency-probe"
          style={[styles.probe, { opacity: on ? 0.5 : 1 }]}
        />
      </Pressable>
      {heavy ? (
        <View style={styles.grid}>
          {Array.from({ length: BURST_CELLS }, (_, index) => (
            <View key={index} style={styles.cell}>
              <Text style={styles.cellText}>
                {on ? index : BURST_CELLS - index}
              </Text>
            </View>
          ))}
        </View>
      ) : null}
    </View>
  );
}

// --- memory: settle, long list scrolled to the end and back, 300 burst commits, unmount, settle ---

type MemSample = { maps?: string; footprintKb: number; heapKb: number; nativeKb?: number; javaKb?: number };
function memSample(): MemSample {
  const m = (NativeModules.LoomBuildConfig as any)?.memSample?.();
  return m ?? { footprintKb: -1, heapKb: -1 };
}

function Memory({ cycles = 1, only }: { cycles?: number; only?: 'list' | 'burst' }) {
  const [phase, setPhase] = useState<'home' | 'list' | 'burst'>('home');
  const [step, setStep] = useState(0);
  const list = useRef<FlatList<number>>(null);
  useEffect(() => {
    let stopped = false;
    let peak = 0;
    const peakTimer = setInterval(() => {
      peak = Math.max(peak, memSample().footprintKb);
    }, 250);
    const wait = (ms: number) => new Promise<void>(r => setTimeout(r, ms));
    const frame = () => new Promise<void>(r => requestAnimationFrame(() => r()));
    (async () => {
      await wait(3000);
      const baseline = memSample();
      const ends: number[] = [];
      const endHeaps: number[] = [];
      let afterList = baseline;
      let afterBurst = baseline;
      let end = baseline;
      for (let cycle = 0; cycle < cycles && !stopped; cycle++) {
        if (only !== 'burst') {
        setPhase('list');
        await wait(1000);
        const ROW = 90;
        const max = SCROLL_DATA.length * ROW;
        for (let dir = 0; dir < 2 && !stopped; dir++) {
          for (let o = 0; o <= max && !stopped; o += 900) {
            list.current?.scrollToOffset({ offset: dir === 0 ? o : max - o, animated: false });
            await frame();
          }
        }
        afterList = memSample();
        }
        if (only !== 'list') {
        setPhase('burst');
        await wait(500);
        for (let i = 1; i <= BURST_COMMITS && !stopped; i++) {
          setStep(i);
          await frame();
        }
        }
      afterBurst = memSample();
      setPhase('home');
      await wait(5000);
      end = memSample();
      ends.push(Math.round(end.footprintKb));
        endHeaps.push(Math.round(end.heapKb));
      }

      clearInterval(peakTimer);
      const kb = (x: number) => Math.round(x);
      report('memory', {
        baselineKb: kb(baseline.footprintKb),
        peakKb: kb(Math.max(peak, afterList.footprintKb, afterBurst.footprintKb)),
        afterListKb: kb(afterList.footprintKb),
        afterBurstKb: kb(afterBurst.footprintKb),
        endKb: kb(end.footprintKb),
        retainedKb: kb(end.footprintKb - baseline.footprintKb),
        endsKb: ends.join(','),
        endsHeapKb: endHeaps.join(','),
        baselineHeapKb: kb(baseline.heapKb),
        endHeapKb: kb(end.heapKb),
        retainedHeapKb: kb(end.heapKb - baseline.heapKb),
        baselineNativeKb: kb(baseline.nativeKb ?? -1),
        endNativeKb: kb(end.nativeKb ?? -1),
        baselineJavaKb: kb(baseline.javaKb ?? -1),
        endJavaKb: kb(end.javaKb ?? -1),
      });
    })();
    return () => {
      stopped = true;
      clearInterval(peakTimer);
    };
  }, []);
  return (
    <View nativeID="bench-ready" style={styles.container}>
      {phase === 'home' ? (
        <View>
          <Text style={styles.header}>Home</Text>
          <Text style={styles.feedText}>Nothing much here.</Text>
        </View>
      ) : null}
      {phase === 'list' ? (
        <FlatList
          ref={list}
          data={SCROLL_DATA}
          keyExtractor={index => String(index)}
          renderItem={({ item }) => <FeedRow index={item} />}
        />
      ) : null}
      {phase === 'burst' ? (
        <View style={styles.grid}>
          {Array.from({ length: BURST_CELLS }, (_, index) => (
            <View key={index} style={styles.cell}>
              <Text style={styles.cellText}>{step % 2 ? index : BURST_CELLS - index}</Text>
            </View>
          ))}
        </View>
      ) : null}
    </View>
  );
}

// memory-view3 / memory-text3 / memory-mixed3: three mount and unmount cycles of 1500 plain Views, Texts
// (one text node each) or FeedRows, to see which node kind leaves memory behind.
function MemoryKind({ kind, hold = false }: { kind: 'view' | 'text' | 'row'; hold?: boolean }) {
  const [on, setOn] = useState(false);
  useEffect(() => {
    let stopped = false;
    const wait = (ms: number) => new Promise<void>(r => setTimeout(r, ms));
    (async () => {
      await wait(3000);
      const baseline = memSample();
      const ends: number[] = [];
      const maps: string[] = [];
      const late: number[] = [];
      const gcd: number[] = [];
      const hists: string[] = [];
      for (let cycle = 0; cycle < 3 && !stopped; cycle++) {
        setOn(true);
        await wait(2000);
        setOn(false);
        await wait(4000);
        const m = memSample();
        ends.push(Math.round(m.heapKb));
        maps.push(m.maps ?? '');
        await wait(8000);
        late.push(Math.round(memSample().heapKb));
        // JS pressure: make Hermes run a few full collections, then see how much native memory
        // (draft nodes owned by dead host objects) came back.
        const keep: any[] = [];
        for (let r = 0; r < 600; r++) {
          keep[r % 24] = new Array(60000).fill(r);
        }
        keep.length = 0;
        await wait(500);
        gcd.push(Math.round(memSample().heapKb));
        if (hold) {
          console.log('[bench] hold ' + cycle);
          await wait(25000);
        }
        hists.push((NativeModules.LoomBuildConfig as any)?.memHist?.() ?? '');
      }
      report('memory', {
        baselineHeapKb: Math.round(baseline.heapKb),
        endsHeapKb: ends.join(','),
        lateHeapKb: late.join(','),
        afterJsGcHeapKb: gcd.join(','),
        maps: maps.join(' | '),
        hists: hists.join(' || '),
      });
    })();
    return () => {
      stopped = true;
    };
  }, []);
  return (
    <ScrollView nativeID="bench-ready" style={styles.container}>
      {on
        ? Array.from({ length: 1500 }, (_, i) =>
            kind === 'view' ? (
              <View key={i} style={{ height: 2 }}>
                <View style={{ height: 1 }} />
              </View>
            ) : kind === 'text' ? (
              <Text key={i} style={{ height: 2 }}>
                {`t${i}`}
              </Text>
            ) : (
              <FeedRow key={i} index={i} />
            ),
          )
        : null}
    </ScrollView>
  );
}

export default function Bench({
  scenario,
  autoscroll = false,
}: {
  scenario: string;
  autoscroll?: boolean;
}) {
  useEffect(() => {
    console.log(`[bench] scenario ${scenario}`);
  }, [scenario]);
  // The windowing transition is an opt-in for measurement: every scenario runs with it off unless
  // its name says `-trans` (e.g. scroll-trans). The renderer's own cost is what the benchmarks track.
  (global as any).__loomListTransitions = scenario.includes('-trans');
  const busy = scenario.endsWith('-busy');
  const base = busy ? scenario.slice(0, -'-busy'.length) : scenario;
  switch (base) {
    case 'memory':
      return <Memory />;
    case 'memory-list3':
      return <Memory cycles={3} only="list" />;
    case 'memory-burst3':
      return <Memory cycles={3} only="burst" />;
    case 'memory-view3':
      return <MemoryKind kind="view" />;
    case 'memory-text3':
      return <MemoryKind kind="text" />;
    case 'memory-row3':
      return <MemoryKind kind="row" />;
    case 'memory-text3h':
      return <MemoryKind kind="text" hold />;
    case 'memory-x3':
      return <Memory cycles={3} />;
    case 'startup':
      return <Startup />;
    case 'scroll':
      return <Scroll busy={busy} autoscroll={autoscroll} />;
    case 'scroll-trans':
      return <Scroll busy={busy} autoscroll={autoscroll} />;
    case 'scroll-blank-trans':
      return <Scroll busy={busy} autoscroll={autoscroll} blank />;
    case 'scroll-blank':
      return <Scroll busy={busy} autoscroll={autoscroll} blank />;
    case 'anim':
      return <Anim native busy={busy} />;
    case 'js-anim':
      return <Anim native={false} busy={busy} />;
    case 'burst-raf':
      return <Burst mode="raf" />;
    case 'transition-heavy':
      return <Heavy transition />;
    case 'heavy-sync':
      return <Heavy transition={false} />;
    case 'burst-unique':
      return <Burst mode="raf" unique />;
    case 'burst-small':
      return <Burst mode="raf" small />;
    case 'burst-resize':
      return <Burst mode="raf" resize />;
    case 'burst-resize-once':
      return <Burst mode="timeout" once />;
    case 'burst-timeout':
      return <Burst mode="timeout" />;
    case 'latency':
      return <Latency busy={busy} heavy={false} snp={false} />;
    case 'latency-heavy':
      return <Latency busy={busy} heavy snp={false} />;
    case 'latency-snp':
      return <Latency busy={busy} heavy={false} snp />;
    default:
      return <Text style={styles.header}>unknown scenario {scenario}</Text>;
  }
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    paddingTop: 40,
    backgroundColor: 'white',
  },
  header: {
    fontSize: 24,
    fontWeight: '700',
    paddingHorizontal: 16,
    paddingBottom: 8,
  },
  feedRow: {
    flexDirection: 'row',
    paddingHorizontal: 16,
    paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: '#d0d7de',
  },
  avatar: {
    width: 40,
    height: 40,
    borderRadius: 20,
    marginRight: 12,
  },
  feedBody: {
    flex: 1,
  },
  feedTitle: {
    fontSize: 15,
    fontWeight: '600',
  },
  feedText: {
    fontSize: 14,
    color: '#57606a',
  },
  animBox: {
    width: 60,
    height: 14,
    marginVertical: 2,
    marginLeft: 16,
    borderRadius: 3,
  },
  grid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
  },
  cell: {
    width: 36,
    height: 24,
    margin: 1,
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: '#eaeef2',
  },
  cellText: {
    fontSize: 11,
  },
  probe: {
    height: 160,
    margin: 16,
    borderRadius: 8,
    backgroundColor: '#1f6feb',
  },
});
