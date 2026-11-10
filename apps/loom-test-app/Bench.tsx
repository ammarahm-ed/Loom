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
  Pressable,
  StyleSheet,
  Text,
  View,
} from 'react-native';

const BUSY_MS = 20;
const BUSY_PERIOD_MS = 50;

function report(scenario: string, data: Record<string, number | string>) {
  console.log(`[bench] result ${JSON.stringify({ scenario, ...data })}`);
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

function Scroll({ busy }: { busy: boolean }) {
  useBusyLoad(busy);
  return (
    <View nativeID="bench-ready" style={styles.container}>
      <FlatList
        data={SCROLL_DATA}
        keyExtractor={index => String(index)}
        renderItem={({ item }) => <FeedRow index={item} />}
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

function Burst({
  mode,
  small = false,
}: {
  mode: 'raf' | 'timeout';
  small?: boolean;
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
      report(`burst-${small ? 'small' : mode}`, {
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
      setTimeout(next, 0);
    }
  }, [mode, step]);

  // burst-small: only the cell the step lands on changes, as in most real updates (a counter, a
  // toggled row). The rest receive identical props and bail out, so React hands the renderer a
  // handful of changed nodes instead of the whole grid.
  if (small && step > 0) {
    smallValues.current[step % BURST_CELLS] = step;
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
          <Text style={styles.cellText}>{(index * 7 + step) % 100}</Text>
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

export default function Bench({ scenario }: { scenario: string }) {
  useEffect(() => {
    console.log(`[bench] scenario ${scenario}`);
  }, [scenario]);
  const busy = scenario.endsWith('-busy');
  const base = busy ? scenario.slice(0, -'-busy'.length) : scenario;
  switch (base) {
    case 'startup':
      return <Startup />;
    case 'scroll':
      return <Scroll busy={busy} />;
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
    case 'burst-small':
      return <Burst mode="raf" small />;
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
