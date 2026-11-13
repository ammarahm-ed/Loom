# Configuration

## Android

Set these in `android/gradle.properties`, or on the command line with `-P<name>=<value>`
(which wins). With none set, the build is stock React Native.

| Property | Values | Effect |
|---|---|---|
| `loom.renderer` | `fabric` (default), `loom` | Which renderer draws the app |
| `loom.js` | `background` (default), `main` | JavaScript on its own thread, or on the Android main thread |
| `loom.worker` | `true`, `false` (default) | Layout, text and diffing on a worker thread. Requires `loom.renderer=loom`; required with `loom.js=background` |
| `loom.profile` | `true`, `false` (default) | Diagnostic C++ and mount timers |
| `loom.cppTests` | `true`, `false` (default) | Build Loom's C++ tests |

The usual configurations:

```properties
# JavaScript on the main thread
loom.renderer=loom
loom.js=main
loom.worker=true

# JavaScript on its own thread
loom.renderer=loom
loom.js=background
loom.worker=true
```

Runtime switches, read when the renderer starts (`adb shell setprop <name> 0|1`, then restart the
app):

| Property | Default | Effect |
|---|---|---|
| `debug.loom.earlycreate` | `1` | A commit that creates many views (a first screen, a navigation) sends their creates to the UI thread as soon as it knows them, so views are built while the worker lays them out |
| `debug.loom.paralleltext` | `0` | Measure a first screen's text on several threads. Off on Android: on a low-end phone the threads it used were the ones the UI thread needed |

## iOS

Environment variables at `pod install` time:

| Variable | Values | Effect |
|---|---|---|
| `RCT_LOOM_RENDERER_IOS` | `1` | Use the Loom renderer |
| `RCT_LOOM_WORKER_IOS` | `1` | Layout, text and diffing on a worker thread (requires the above) |
| `RCT_LOOM_JS_IOS` | `main` (default), `background` | JavaScript on the main thread, or on its own thread (`background` requires the worker) |

```bash
# JavaScript on its own thread
RCT_LOOM_RENDERER_IOS=1 RCT_LOOM_WORKER_IOS=1 RCT_LOOM_JS_IOS=background pod install
```

Runtime switches (`NSUserDefaults`, also accepted as launch arguments such as
`-LoomAsyncText NO`):

| Key | Default | Effect |
|---|---|---|
| `LoomAsyncText` | `YES` | Draw paragraph text on a background queue when it changes inside a box of the same size; see [Asynchronous text on iOS](/guide/async-text) |
| `LoomAsyncTextFirstDraw` | `YES` | Draw a paragraph's first appearance in the background too |
| `LoomEarlyCreate` | `YES` | A commit that creates many views (a first screen, a navigation) sends their creates to the main thread as soon as it knows them, so views are built while the worker lays them out |
| `LoomParallelText` | `YES` | A commit that creates many paragraphs measures their text on several cores before the real layout pass |

## Debug-only settings

| Setting | Effect |
|---|---|
| `adb shell setprop debug.loom.jank_budget_ms N` | Jank report threshold (default 50 ms) |
| `adb shell setprop debug.loom.jank 0` | Turn the jank reporter off |
