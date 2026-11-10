# Main-thread jank reporter (debug builds only)

With JavaScript on the UI thread (`RN_UI_THREAD`), any slow task drops frames. In **debug builds** Resha
tells the developer, in the JS console, what blocked the main thread, for how long and where.

It is compiled out of release builds entirely: C++ is guarded by `RN_JANK_REPORTER`
(`RN_UI_THREAD && !NDEBUG`), Java by `BuildConfig.DEBUG`, JS by `__DEV__`. There is no watchdog thread, no
per-task bookkeeping and no switch to turn it on in release.

## Why the default budget is 50 ms

A debug build runs React's dev build, an unoptimized bundle and unoptimized C++, so it is several times slower
than release. A 16 ms bar would flag work that is comfortably fast in release. The default is the web's
long-task threshold, **50 ms**, used for every report and for DevTools `longtask` entries. Every message says it
was measured in a debug build. Work anywhere near the budget in debug is likely to drop frames in release too.

## What is reported

| Source | Detects | Where |
| --- | --- | --- |
| JS task | a scheduler task (event handler, timer, rAF, React render/commit) over budget, split into JS / microtask / rendering time | `RuntimeScheduler_Modern::runEventLoopTick` |
| Slow frame | a frame over budget, with its longest phase (input, animation callbacks, layout, draw, sync, GPU, swap, delay before the frame) | `Window.addOnFrameMetricsAvailableListener` (`JankReporterSupport.kt`); also in `js` mode |
| View apply | `UIThreadDirectViewApplier.applyBatch` over budget, with the batch size | `UIThreadDirectViewApplier.kt` |

JS task reports name the cause (event type, `setTimeout`/`setInterval`/`requestAnimationFrame`, React
render/commit with native commit count and time). Timer callbacks also say where they were scheduled
(`setUpJankReporter.js` keeps the `Error` captured at registration and reads its `stack` only if the callback
ran long).

**Stacks are sampled while the task runs.** A watchdog thread checks (every 10 ms) whether a task has passed the
budget. Only then it turns on Hermes's sampling profiler (1 kHz) and samples the main thread's Java stack every
4 ms (at most 10 samples). When the task ends it turns the profiler off and reports the hottest JS functions
(`name (file:line:col)`) and, if the main thread was in app
Java code, the most frequent top Java frames. Tasks that finish in time pay two atomic stores and a clock read.

Reports reach the console from a **later** task (`console.warn`; Java-originated ones via a `ReshaJank` device
event), never from inside the slow one. At most one report per second is shown, with a count of suppressed ones.

```
[Resha] Main thread blocked 181 ms (debug build; release is usually several times faster) by a React
render/commit: 181 ms JS, 0 ms microtasks, 0 ms rendering. If it is anywhere near this long in release it will
drop frames; split the work or move it off the main thread (e.g. a worker).
  Included 1 native commit(s) taking 2 ms.
  JS (sampled while blocked):
    38% (anonymous) (index.bundle:26324:19)
    10% completeWork (index.bundle:13399:5)
```

## Settings (debug only)

```
adb shell setprop debug.resha.jank_budget_ms 30   # budget in ms (default 50)
adb shell setprop debug.resha.jank 0              # turn the reporter off
```

Read once at process start. DevTools `longtask` entries use the same budget while the reporter is on.

## Limits

- JS stacks need Hermes's sampling profiler (on in RN's Hermes config). If it is unavailable the report simply
  has no JS section.
- The sampled Java stack is empty while the main thread is only inside the Looper/JNI plumbing, which is the
  case for pure JS work.
- JS source positions are mapped to source files through Metro's `/symbolicate` (the endpoint LogBox uses) in the `__reshaJankReport` JS hook; LogBox itself cannot be given a custom stack for a warning, so the message text is rewritten. Without Metro the bundle positions are shown. React component names are not included.
- In `js` mode (JS on its own thread) only the frame and apply reports apply.
