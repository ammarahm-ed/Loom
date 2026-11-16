# Benchmarks

All numbers are from Release builds on real devices. "Stock" is React Native 0.88.1 without
Loom. Each scenario runs once per build; startup is the median of several cold starts with the
builds alternated. Differences of a millisecond or two, or a few percent of CPU, are within
noise. The harness lives in `apps/loom-test-app/bench` (`run.js` for Android, `run-ios.js` for
iOS).

**How to read them.**
- *Per update*: wall time of one React update in a burst of 300, from `setState` to the next
  update starting. A value at or below the display's frame time (16.7 ms at 60 Hz, 8.3 ms at
  120 Hz) means updates keep up with the display.
- *CPU*: CPU time per thread over the scenario. With JavaScript on the main thread, main-thread
  CPU includes JavaScript; compare it with stock's main + JS threads.
- *Dropped frames / jank*: frames that missed their vsync. On Android, frames drawn on time but
  presented a vsync late (buffer stuffing) are not counted as dropped.
- *Startup*: cold starts only, in milliseconds from the process's creation. See
  [How startup is measured](#how-startup-is-measured).

## Low-end Android

Infinix X6516: Unisoc SC9863A (8 × Cortex-A55), 1.9 GB RAM, 32-bit (armeabi-v7a), Android 12,
60 Hz. Installed the way a store installs apps (baseline profile and ahead-of-time compilation).
This phone's speed drifts 15-20% with temperature, so builds are always alternated.

| | Stock | Loom, JS on its own thread | Loom, JS on the main thread |
|---|---|---|---|
| Startup: time to initial display | 532 ms | **487 ms** | **487 ms** |
| Startup: first content | 765 ms | **501 ms** (-34%) | 590 ms (-23%) |
| 300-cell re-render, per update | 283.8 ms | **91.8 ms** (3.1x faster) | 134.2 ms (2.1x) |
| 300-cell re-render, total CPU | 109.0 s | **73.2 s** (-33%) | 73.0 s (-33%) |
| One cell of 300 changes, per update | 49.8 ms | 33.0 ms | **32.7 ms** |
| Scrolling: time per new-rows update | 134.8 ms | **52.5 ms** | time-sliced |
| Scrolling: dropped frames | 7 | **0** | 45 |
| Scrolling: memory (PSS) | 104 MB | 86 MB | **83 MB** |
| Tap latency, median / p90 | 29.5 / 34.1 ms | 25.5 / 33.1 ms | **about 19 / 25 ms** |
| Native animation: main-thread CPU | 6.17 s | 4.81 s (-22%) | **4.59 s** (-26%) |

On a phone this slow, JavaScript on the main thread competes with scrolling for the only thread
that draws; Loom time-slices non-urgent renders in that mode so frames can still be drawn.
JavaScript on its own thread keeps scrolling smooth and is the fastest for large updates. With
JavaScript on its own thread, the first frame already contains React content.

## High-end Android

OnePlus 9 (LE2115): Snapdragon 888, arm64-v8a, Android 14, 120 Hz. Store-like install. Startup is
the median of 5 rounds.

| | Stock | Loom, JS on its own thread | Loom, JS on the main thread |
|---|---|---|---|
| Startup: time to initial display | 236 ms | 229 ms | **211 ms** |
| Startup: first content | 208 ms | **185 ms** (-11%) | 199 ms |
| 300-cell re-render, per update | 40.0 ms | **15.1 ms** (2.6x faster) | 21.1 ms |
| 300-cell re-render, total CPU | 15.9 s | 8.6 s | **7.8 s** (-51%) |
| One cell of 300 changes, per update | 10.7 ms | 8.8 ms | **8.4 ms** |
| 1000-cell update, time to land | 93.1 ms | **29.3 ms** (3.2x faster) | 40.8 ms |
| 1000-cell transition, time to land | 92.2 ms | **29.9 ms** | 40.3 ms |
| Scrolling: time per new-rows update | 21.8 ms | **10.5 ms** | 14.1 ms |
| Scrolling: jank | 0.4% | **0.2%** | 2.8% |
| Scrolling with busy JavaScript: jank | 1.3% | **0.2%** | 28.8% |
| Scrolling: memory (PSS) | 199 MB | **148 MB** | 150 MB |
| Tap latency, median / p90 | 18 / 20 ms | 21 / 26 ms | **15.5 / 19 ms** |
| Tap latency with busy JavaScript, median / p90 | 20.5 / 37.1 ms | 18 / 25 ms | **11 / 14.1 ms** |
| Native animation with busy JavaScript, p90 frame | **8 ms** | **8 ms** | 18 ms |
| JS-driven animation, total CPU | 16.4 s | 12.0 s | **9.9 s** (-40%) |

At 120 Hz a frame lasts 8.3 ms, so with JavaScript on the main thread any JavaScript task longer
than that costs a frame. JavaScript on its own thread keeps every scenario at or above stock's
frame rate. JavaScript on the main thread answers taps fastest, because a touch reaches
JavaScript without crossing threads.

## iOS

Scrolling is driven from JavaScript; tap latency is not measured on iOS (devices cannot be tapped
remotely).

### iPhone XS Max

A12, 60 Hz. Startup is the median of 5 cold launches.

| | Stock | Loom, JS on its own thread | Loom, JS on the main thread |
|---|---|---|---|
| Startup: first screen mounted | 104.1 ms | **101.2 ms** | 101.3 ms |
| Startup: first screen's `useEffect` (react-native-performance TTI) | 94.3 ms | 86.7 ms | **84.7 ms** |
| 300-cell re-render, per update | **16.8 ms** | **16.7 ms** | 19.4 ms |
| 300 cells with new text, per update | 34.1 ms | **16.8 ms** (2x faster) | 17.1 ms |
| 300 cells with new text: main-thread CPU | 9.7 s (+ 9.8 s JS) | **2.2 s** (+ 1.9 s JS) | 3.8 s (JS included) |
| 300 cells with new text: total CPU | 20.1 s | 10.1 s | **8.8 s** (-56%) |
| 300 cells resizing their text, per update | 37.6 ms | **16.8 ms** | 42.8 ms |
| 1000-cell update, time to land | 80.0 ms | **62.2 ms** | 64.7 ms |
| 1000-cell update, jank | 7.5% | **4.4%** | 13.3% |
| 1000-cell transition, time to land | 83.6 ms | 65.3 ms | **64.7 ms** |
| JS-driven animation, total CPU | 5.4 s | 3.1 s | **3.0 s** (-45%) |
| JS-driven animation with busy JavaScript, total CPU | 7.1 s | 2.6 s | **2.6 s** (-64%) |
| Native animation with busy JavaScript, 90th percentile frame | 16.7 ms | **16.7 ms** | 20.7 ms |
| Scrolling, jank | 0.2% | **0%** | 4.5% |

### iPhone 15 Pro Max

A17 Pro, ProMotion. The test app does not opt in to frame rates above 60 Hz
(`CADisableMinimumFrameDurationOnPhone`), so it draws at 60 Hz; frame percentiles are given
instead of jank. Startup is the median of 6 cold launches per build, each Loom build alternated
with stock.

| | Stock | Loom, JS on its own thread | Loom, JS on the main thread |
|---|---|---|---|
| Startup: first screen mounted | 100.8 ms | **97.6 ms** | 104.9 ms |
| Startup: first screen on the display | 111.0 ms | **97.7 ms** | 105.1 ms |
| Startup: first screen's `useEffect` (react-native-performance TTI) | 90.8 ms | **84.2 ms** | 91.0 ms |
| 300-cell re-render, total CPU | 6.2 s | 5.7 s | **3.5 s** (-44%) |
| 300 cells with new text, main-thread CPU | 3.7 s (+ 4.2 s JS) | **1.8 s** (+ 1.3 s JS) | 2.7 s (JS included) |
| 300 cells resizing their text, per update | 18.8 ms | **16.7 ms** | 16.8 ms |
| 300 cells resizing their text, total CPU | 11.2 s | 7.4 s | **4.8 s** (-57%) |
| 1000-cell update, time to land | 60.6 ms | **41.6 ms** | **41.6 ms** |
| 1000-cell transition, time to land | 60.6 ms | 43.7 ms | **42.4 ms** |
| JS-driven animation with busy JavaScript, total CPU | 4.6 s | **2.2 s** (-52%) | 2.3 s |
| Native animation with busy JavaScript, 90th percentile frame | **16.7 ms** | **16.7 ms** | 20.3 ms |
| Scrolling with busy JavaScript, 90th percentile frame | 16.8 ms | **16.7 ms** | 19.8 ms |

### What affects the iOS numbers

- Paragraph text is drawn on a background queue whenever it changes inside a box of the same
  size, so the main thread spends far less time on text; see
  [Asynchronous text on iOS](/guide/async-text). Text that changes size is drawn on the main
  thread, which is why resizing text costs more frames with JavaScript on the main thread.
- A commit that creates many views (a first screen, a navigation) sends their creates and props
  to the main thread as soon as the worker knows them. The main thread builds the views while
  the worker lays them out, and the commit's own batch only inserts and positions them.
- A commit that creates many paragraphs runs Yoga once with estimated text sizes, measures the
  text for the constraints that pass asked for on several cores, then lays out for real.
- Views with the same solid border share one stretchable border image instead of drawing one
  each.

With JavaScript on its own thread, Loom matches or beats stock everywhere. With JavaScript on the
main thread, JavaScript, view updates and text that changes size share one thread, so large
updates cost more frames.

## How startup is measured

Following [Measure TTI](https://github.com/callstackincubator/agent-skills/blob/main/skills/react-native-best-practices/references/native-measure-tti.md):
cold starts only (prewarmed and background launches are dropped), milliseconds from the
process's creation. The iOS harness records each step: pre-main done, `didFinishLaunching`,
bundle evaluated, React Native's content-appeared notification, the first screen's `useEffect`
(react-native-performance's `screenInteractive`), the end of the main-thread mount of the first
screen, and the Core Animation commit that contains it.

*Mounted* is the startup number to compare. The `useEffect` mark is taken when React commits,
which is before layout with Loom (its worker lays out afterwards) but after layout with stock
React Native, so it flatters Loom. The commit that reaches the display is either right after the
mount or one frame later, depending on where the mount ends relative to the display's refresh,
which adds up to one frame of noise to single launches.

On Android, *first content* is the first frame drawn with React content (a pre-draw listener),
and *time to initial display* is what `am start -W` reports. On Android, the early view creation
described above applies as well; parallel text measurement is off by default there
(`adb shell setprop debug.loom.paralleltext 1` turns it on).
