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
| Startup: time to initial display | 511 ms | **481 ms** | **478 ms** |
| Startup: first content | 724 ms | **509 ms** (-30%) | 586 ms (-19%) |
| Startup: memory (PSS) | **42.8 MB** | 44.9 MB | 43.9 MB |
| 300-cell re-render, per update | 209.5 ms | **66.9 ms** (3.1x faster) | 125.7 ms (1.7x) |
| 300-cell re-render, total CPU | 81.2 s | **44.6 s** (-45%) | 47.9 s (-41%) |
| One cell of 300 changes, per update | 38.4 ms | **17.1 ms** | 33.2 ms |
| 1000-cell transition, time to land | 408 ms | **208 ms** | 301 ms |
| 1000-cell heavy update, time to land | 603 ms | **190 ms** (3.2x faster) | 478 ms |
| Scrolling: time per new-rows update | 102 ms | **55 ms** | 264 ms |
| Scrolling: jank | 6.4% | **3.1%** | 11.1% |
| Scrolling with busy JavaScript: jank | 8.2% | **0.3%** | 12.4% |
| Scrolling: memory (PSS) | 106.6 MB | 77.8 MB | **71.7 MB** |
| Scrolling: native heap | 57.1 MB | 22.6 MB | **22.5 MB** |
| Tap latency, median / p90 | 29.5 / 35.1 ms | 28.0 / 35.2 ms | **22.5 / 29.0 ms** |
| Tap latency with busy JavaScript, median / p90 | 34.5 / 46.1 ms | **27 / 33.1 ms** | 28 / 29.4 ms |
| Native animation: main-thread CPU | 4.23 s | 4.05 s | **4.02 s** |
| Native animation with busy JavaScript: main-thread CPU | 4.25 s | **4.02 s** | 5.14 s |
| JS-driven animation: jank | **0%** | 0.4% | 93% |
| JS-driven animation with busy JavaScript: jank | 15.1% | **0.2%** | 90.9% |
| JS-driven animation: total CPU | 15.5 s | 18.8 s | **9.5 s** (-39%) |

Memory over a mount, scroll and unmount cycle, from the same run:

| | Stock | Loom, JS on its own thread | Loom, JS on the main thread |
|---|---|---|---|
| Idle baseline | **36.3 MB** | 37.8 MB | 37.6 MB |
| Peak | 171 MB | **101 MB** | 111 MB |
| After 300 commits | 112 MB | **97 MB** | 104 MB |
| Still held after unmount and settle | +73.6 MB | **+54.1 MB** | +62.0 MB |
| Native heap still held after unmount | +29.1 MB | **+13.7 MB** | +15.7 MB |
| Three mount/unmount cycles, memory at the end of each | crashed | 88.7, 72.7, 68.4 MB | 87.2, 70.8, 71.3 MB |
| 1500 views, three cycles, heap at the end of each | crashed | 78.6, 78.7, 78.7 MB | 78.6, 78.7, 78.7 MB |
| 1500 views x3, after a JavaScript GC | crashed | 71.5, 71.6, 71.5 MB | 71.5, 71.6, 71.5 MB |

Neither Loom build grows after the first cycle. Stock React Native crashed in both repeated
mount/unmount scenarios, every time it ran them (3 of 3 and 1 of 1): `addViewAt` failed to insert a
view at an index one past the parent's child count
(`IndexOutOfBoundsException` in `ViewGroup.addView`, from `SurfaceMountingManager.addViewAt`, with
view clipping on large lists). Loom completes both.

All numbers are from one run per scenario, with the three builds measured back to back; startup
is the median of 3 cold starts. The phone's speed drifts 15-20% with temperature, so compare
columns of the same run, not numbers across tables. The 1000-cell cases drop frames on every build
(100% jank); time to land is what separates them.

On a phone this slow, JavaScript on the main thread competes with scrolling for the only thread
that draws, and it costs the most when JavaScript animates: 93% of frames are janky with
JS-driven animation, against 0.4% with JavaScript on its own thread. JavaScript on its own thread
keeps scrolling and animation smooth and is the fastest for large updates. With JavaScript on the
main thread, taps are answered soonest because a touch reaches JavaScript without crossing
threads. With JavaScript on its own thread, the first frame already contains React content.

## Older flagship Android

Sony SO-01M (Xperia 5): Snapdragon 855 (8 cores), 6 GB RAM, arm64-v8a, Android 11, 60 Hz (the
active display mode; the panel allows up to 120). Each scenario ran once, with the three builds
measured back to back; startup is the median of 3 cold starts.

| | Stock | Loom, JS on its own thread | Loom, JS on the main thread |
|---|---|---|---|
| Startup: time to initial display | 152 ms | 146 ms | **141 ms** |
| Startup: first content | 203 ms | **155 ms** (-24%) | 171 ms (-16%) |
| Startup: memory (PSS) | 85.6 MB | **84.9 MB** | **84.9 MB** |
| 300-cell re-render, per update | 63.3 ms | **17.1 ms** (3.7x faster) | 35.2 ms (1.8x) |
| 300-cell re-render, total CPU | 22.0 s | **8.2 s** (-63%) | 11.4 s (-48%) |
| 300-cell re-render, main-thread CPU | 6.9 s | **2.6 s** | 7.1 s |
| One cell of 300 changes, per update | 16.8 ms | **16.7 ms** | **16.7 ms** |
| 1000-cell transition, time to land | 99.3 ms | **35.4 ms** (2.8x faster) | 49.0 ms |
| 1000-cell heavy update, time to land | 96.6 ms | **30.3 ms** (3.2x faster) | 52.3 ms |
| Scrolling: time per new-rows update | 26.9 ms | **13.1 ms** | 36.1 ms |
| Scrolling: jank | **0%** | **0%** | **0%** |
| Scrolling with busy JavaScript: jank | 0.1% | **0.1%** | 7.1% |
| Scrolling: memory (PSS) | 201.1 MB | 147.6 MB | **145.4 MB** |
| Scrolling: native heap | 110.4 MB | 34.0 MB | **33.9 MB** |
| Tap latency, median / p90 | 32.5 / 37.2 ms | 20 / 27.0 ms | **15.5 / 24.1 ms** |
| Tap latency with busy JavaScript, median / p90 | 36.5 / 50.0 ms | **20.5 / 27.1 ms** | 23.5 / 33.2 ms |
| Native animation: main-thread CPU | 1.63 s | 1.33 s | **1.28 s** |
| Native animation with busy JavaScript: main-thread CPU | 1.24 s | **0.93 s** | 2.90 s |
| JS-driven animation: jank | 5.1% | **0%** | 5.4% |
| JS-driven animation with busy JavaScript: jank | 40% | **0%** | 34.8% |
| JS-driven animation: total CPU | 12.8 s | 6.0 s | **5.1 s** |

Memory over a mount, scroll and unmount cycle:

| | Stock | Loom, JS on its own thread | Loom, JS on the main thread |
|---|---|---|---|
| Idle baseline | 41.4 MB | **41.0 MB** | **40.9 MB** |
| Peak | 229.1 MB | **121.4 MB** | 129.5 MB |
| After 300 commits | 159.0 MB | **113.1 MB** | 117.0 MB |
| Still held after unmount and settle | +116.1 MB | **+69.6 MB** | +76.9 MB |
| Native heap still held after unmount | +39.5 MB | **+16.3 MB** | +16.9 MB |
| Three mount/unmount cycles, memory at the end of each | 161.4, 163.7, 160.5 MB | 116.4, 98.7, 103.6 MB | 116.2, 102.7, 101.9 MB |
| 1500 views, three cycles, heap at the end of each | 22.8, 23.0, 22.9 MB | **13.1, 13.1, 13.1 MB** | **13.1, 13.1, 13.1 MB** |
| 1500 rows, three cycles, heap at the end of each | 136.0, 136.6, 136.6 MB | **95.5, 95.8, 95.6 MB** | 95.4, 95.8, 95.6 MB |

No build grows after the first cycle, and none crashed on this phone (stock React Native did crash
on the Infinix above).

The one-cell update is one 60 Hz frame on every build, so that row measures the display, not the
renderer. Tap latency is mostly the display too: a 60 Hz frame lasts 16.7 ms, and the last stage
of a tap (the wait for the next frame) takes 7-13 ms on every build. The 1000-cell cases drop
frames on every build; Loom lands the update 2-3x sooner but reports more janky frames (for the
heavy update 87.8% and 86.8% against 32.1% for stock), so compare time to land there, not jank.
JavaScript on the main thread costs the most when JavaScript is busy: scrolling jank 7.1%, native
animation CPU 2.90 s against 1.24 s on stock.

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

### Use native marks, not JavaScript ones

Compare renderers with two marks taken natively, on the thread that owns the views:

- **Mounted:** the last startup mount batch has been applied to the native views.
- **Displayed:** the first frame drawn after that batch (the Core Animation commit on iOS, the
  next drawn frame on Android and desktop hosts).

A JavaScript timestamp, whether from `useEffect`, `requestAnimationFrame` or a
`performance.now()` in a component, measures when React finished, not when the user can see the
screen. With stock React Native, layout runs during React's commit, so the gap between that
timestamp and the mount is small. With Loom, React's commit hands the tree to the worker, which
lays it out afterwards, and the UI thread then applies the batch. The JavaScript mark therefore
fires before Loom has done its layout and mount work, and it can fire tens of milliseconds early.
On a desktop host (react-native-gpui, 5k-cell grid, JavaScript on its own thread), a JavaScript
first-frame mark made Loom's first frame look 27% earlier than it was. A JavaScript mark is still
useful for comparing two Loom builds, but not for comparing Loom with stock React Native.

When you port the harness to a new host, put the *mounted* mark at the end of the host's
`applyBatch` for the startup surface's last startup batch, and the *displayed* mark in the
host's frame-presented callback that follows it.
