# Choosing a mode

Loom runs your JavaScript in one of two places: on the **main thread**, the thread that
handles touches and draws the screen, or on a **background JS thread** of its own. Both use the
Loom renderer and run the same JavaScript. The choice decides one thing: what the user sees
when your JavaScript is slower than a frame.

```properties
# android/gradle.properties (iOS: ios/Podfile.properties.json, same key)
loom.js=background   # default
loom.js=main
```

## The short version

| | Main thread (`loom.js=main`) | Background JS thread (`loom.js=background`) |
|---|---|---|
| Tap to visible change | Same frame, like a native app (about 14-16 ms at 60 Hz) | Usually one frame later (about 18-23 ms) |
| When JavaScript is slower than a frame | The frame waits: **jank** | Frames stay on time, content is late: **stale or blank content** |
| What the screen shows | Always consistent with your state | Can trail your state by a frame or more |
| Scrolling and native animations while JavaScript is busy | Pause with it | Keep running |
| Large `startTransition` renders | Sliced around frames, never late | Run beside frames |
| Large urgent renders, long handlers | Block frames until done | Never block frames |
| `setState` vs `setNativeProps` | Same speed | `setNativeProps` is faster |
| Native code calling React synchronously | Yes | No |
| Closest to | A native app | React Native's default threading |

Neither mode makes slow JavaScript free. Main-thread mode shows it as **late frames**,
background mode as **late content**. The rest of this page shows why, and how to keep
JavaScript fast enough that neither happens.

## A frame, and who waits for whom

A phone redraws the screen at a fixed rate: every 16.7 ms at 60 Hz, every 8.3 ms at 120 Hz.
Each refresh, the main thread must have finished its work (input, applying view changes) and
drawn. Work that runs past a refresh pushes that frame back.

### A tap

In main-thread mode the touch, your handler, React's render and commit, and applying the views
all happen on the main thread before the next refresh:

<FrameTimeline name="main-tap" />

In background mode the touch crosses to the JS thread, the commit is laid out, and the view
changes come back to the main thread. The main thread doesn't wait for them, so the change
usually lands one refresh later:

<FrameTimeline name="bg-tap" />

### Slow JavaScript: jank or stale content

Say an event handler, or the render it triggers, takes 30 ms: about two frames. In
main-thread mode the main thread is busy for those 30 ms and cannot draw:

<FrameTimeline name="main-long" />

The screen freezes for a frame (a dropped frame, or jank) and touches wait too. When it
draws again, everything is up to date. Native apps behave the same way when their main thread
does too much: a slow table cell drops a frame, it never shows an empty row.

In background mode the main thread is free, so it keeps drawing on time, but it can only draw
what it has:

<FrameTimeline name="bg-long" />

Scrolling, native animations and Reanimated's UI-thread animations stay smooth. But for those
frames the content is out of date: a tap's highlight shows up late, and in a list being
flung faster than JavaScript renders, rows scroll in **blank** and fill in a moment later.
Native apps almost never show blank rows; this is the price of rendering away from the
thread that draws.

Background mode is never forced to wait. Main-thread mode never shows stale content. There is
no third option that guarantees both: guaranteeing that content lands in the frame means the
frame must wait for JavaScript, and that is main-thread mode.

## What Loom does on the main thread

In main-thread mode Loom's scheduler knows when the next refresh is due:

- **Non-urgent work is sliced.** A `startTransition` or `useDeferredValue` render runs in
  slices of about 5 ms. A slice also stops early when the refresh is near, and the next slice
  starts only after the frame has drawn. A large transition lands a few frames later; no
  frame is late.

  <FrameTimeline name="main-sliced" />

- **Urgent work is never split.** Event handlers, plain `setState` outside a transition,
  `flushSync` and synchronous requests from native code run to completion, because the user is
  waiting for their result. This is where main-thread jank comes from.
- **One JavaScript function is never interrupted.** A 30 ms loop is 30 ms on the main thread,
  in any engine.
- **The renderer's own work moves off.** Loom's worker thread does layout, text measurement
  and diffing for large commits (small ones run inline, where a handoff would cost more). The
  main thread is left with your JavaScript and applying views. See
  [What is Loom?](/guide/what-is-loom#threads).
- **Debug builds report blocks.** The [jank reporter](/guide/jank-reporter) names every task
  that blocked the main thread, with sampled JavaScript stacks, and shows them as `longtask`
  entries in React Native DevTools.

## Tricks for main-thread mode

The rule is the one native developers follow: **no single piece of main-thread work longer
than a few milliseconds.** Total JavaScript time matters less than the longest uninterrupted
stretch.

### Make large updates non-urgent

Keep the urgent part of an interaction tiny and put the rest in a transition, which Loom
slices around frames:

```tsx
import {startTransition} from 'react';

function onFilterChange(query: string) {
  setQuery(query); // urgent and small: the input updates this frame
  startTransition(() => {
    setResults(filter(allItems, query)); // large: rendered in slices
  });
}
```

For values derived during render, `useDeferredValue` does the same without restructuring:

```tsx
const deferredQuery = useDeferredValue(query);
const results = useMemo(() => filter(allItems, deferredQuery), [deferredQuery]);
```

Avoid `flushSync`: it turns a sliceable update into an urgent one.

### Move heavy JavaScript to a worker

Parsing large JSON, sorting or searching big lists, crypto, diffing data, image or text
processing: none of it needs the UI. Run it in a JavaScript worker with
[`@ammarahmed/react-native-workers`](https://www.npmjs.com/package/@ammarahmed/react-native-workers)
and send back only the result:

<FrameTimeline name="main-workers" />

Messages between threads are copied, so send compact inputs and results rather than
streaming large objects back and forth every frame, and keep a few long-lived workers rather
than starting one per task.

### Split long loops yourself

If work must run in the main JavaScript runtime, process it in chunks and give the thread
back between them:

```ts
async function processInChunks<T>(items: T[], work: (item: T) => void, budgetMs = 4) {
  let i = 0;
  while (i < items.length) {
    const start = performance.now();
    while (i < items.length && performance.now() - start < budgetMs) {
      work(items[i++]);
    }
    await new Promise(resolve => setTimeout(resolve, 0)); // let a frame draw
  }
}
```

### Keep list items cheap

Lists are where main-thread mode is tested hardest, because new rows render while the user
scrolls.

- Memoize row components (`React.memo`) and keep `renderItem` free of heavy computation:
  prepare the data before it reaches the list.
- Give rows a fixed or known size (`getItemLayout`, or a list like FlashList with size
  estimates) so the list doesn't measure as it goes.
- Render ahead of the viewport (`windowSize`, FlashList's `drawDistance`) so rows are ready
  before they scroll in, the way native lists prefetch cells.
- Use stable keys so React updates rows instead of recreating them.

### Animate without JavaScript per frame

A JavaScript callback on every frame shares the main thread with everything else. Run
animations on the UI runtime with Reanimated (see [Reanimated](/guide/reanimated)) or with the
native driver. In main-thread mode a plain `setState` is as fast as `setNativeProps`
([Instant updates](/guide/instant-updates)), but per-frame animation still belongs off the
JavaScript path.

### Measure in release

Debug builds run JavaScript several times slower, which is why the jank reporter's default
budget is 50 ms. Use it to find the slowest tasks, then confirm in a release build: a 40 ms
debug render is often under a frame in release.

## Tricks for background mode

The same work that causes jank on the main thread causes stale content here, so most of the
tricks above apply too. Specific to background mode:

- **Render lists further ahead.** A larger `windowSize` or `drawDistance` gives the JS thread
  time to fill rows before they scroll in. Placeholders or skeleton rows hide what is still
  missing.
- **Use `setNativeProps` or Reanimated for latency-critical visuals.** They skip the trip
  through the JS thread, so a press highlight or drag follows the finger in the same frame.
- **Keep the JS thread free during gestures.** A long task on the JS thread delays every
  touch handler and every commit behind it, even though the screen keeps moving.

## Which to pick

**Main-thread mode** when your app should feel like a native one: taps that answer in the same
frame, content that is never stale, native components that call into React synchronously. It
asks for discipline about main-thread work, which the slicing, the workers and the jank
reporter help with.

**Background mode** (the default) when your app runs long synchronous JavaScript you can't
split or move to a worker, or depends on libraries that do heavy JavaScript during
interactions. Scrolling and animations stay smooth no matter what JavaScript does; content can
trail behind.

The full native-style setup is main-thread mode with the Loom worker on (the default) and heavy
logic in JavaScript workers: the main thread only renders and applies views, the Loom worker
lays out, and workers do the rest.

## Switching

`loom.js` is read at startup, before the JavaScript thread is created, and stays fixed for the
app's lifetime; see [Configuration](/reference/configuration). One prebuilt Loom serves both
modes, so switching recompiles no React Native or Loom code: change `loom.js` in
`ios/Podfile.properties.json` or `android/gradle.properties`, then run `pod install` or rebuild
the app. Debug builds can also switch at launch without a rebuild (`-LoomJSThread main` on
iOS, `adb shell setprop debug.loom.js main` on Android). Desktop and custom hosts call
`LoomHost::setJavaScriptOnUIThread()` once before the first surface starts. The same
JavaScript runs in both modes.

Measured on a low-end Android phone and on an iPhone, background mode is faster for large
updates and keeps scrolling smooth; main-thread mode gives the fastest taps. See
[Benchmarks](/benchmarks).
