# Finding main-thread jank

In main-thread mode, anything that blocks the main thread for longer than a frame delays
drawing. Debug builds watch for it and tell you what happened.

```
[Loom] Main thread blocked 181 ms (debug build; release is usually several times faster) by a
React render/commit: 181 ms JS, 0 ms microtasks, 0 ms rendering. If it is anywhere near this
long in release it will drop frames; split the work or move it off the main thread.
  JS (sampled while blocked):
    38% flushMutationEffects (ReactFabric-dev.js:15703:4)
    10% completeWork (ReactFabric-dev.js:13399:5)
```

What you get:

- The cause: an event handler, a timer or animation-frame callback (with where it was
  scheduled), a React render, or native work such as applying views.
- JavaScript stacks sampled while the work was running, mapped to your source through Metro.
- Java stacks for native work.
- The same blocks as `longtask` entries in React Native DevTools.

## Settings

The reporter exists only in debug builds; release builds contain none of its code.

```sh
adb shell setprop debug.loom.jank_budget_ms 100   # report blocks longer than 100 ms (default 50)
adb shell setprop debug.loom.jank 0               # turn it off
```

The default budget is 50 ms rather than one frame because debug builds run JavaScript and
native code several times slower than release.
