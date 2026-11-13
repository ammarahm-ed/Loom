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

Runtime settings have their own section below. The `debug.loom.*` system properties still work as
overrides (`adb shell setprop <name> <value>`, then restart the app).

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

Runtime settings have their own section below. `NSUserDefaults` keys, also accepted as launch
arguments such as `-LoomAsyncText NO`, still work as overrides.

## Runtime settings

The build switches above are fixed at compile time. These settings can change while the app runs:

```js
import {Loom} from 'react-native-loom';

Loom.asyncText = false;
Loom.jankBudgetMs = 30;
Loom.jsOnMainThread; // read-only build info
```

Each setting is a native atomic read with a relaxed load, so a read costs what a constant would. A
write applies to work that starts afterwards: the next text draw, the next commit. Nothing is done
per node. Two settings matter mostly at startup (`earlyCreate`, `asyncTextFirstDraw`); put the
write at the top of `index.js`, before the first screen renders, to cover them.

### Resolution order

At startup the host resolves each setting in this order, each step replacing the last:

1. the built-in default;
2. the app's native config: iOS `Info.plist`, a `Loom` dictionary keyed by setting name; Android,
   `<meta-data android:name="loom.<name>" android:value="...">` in `AndroidManifest.xml`;
3. the legacy overrides: iOS `NSUserDefaults` (including launch arguments) and the
   `LOOM_PROFILE_WORKER` environment variable; Android `debug.loom.*` system properties;
4. JavaScript writes, at any time after.

```xml
<application ...>
  <meta-data android:name="loom.parallelText" android:value="true" />
  <meta-data android:name="loom.jankBudgetMs" android:value="30" />
</application>
```

```xml
<!-- Info.plist -->
<key>Loom</key>
<dict>
  <key>asyncText</key><false/>
  <key>jankBudgetMs</key><integer>30</integer>
</dict>
```

Rules for the JS object:

- An unknown name throws. Assigning to a read-only value throws a `TypeError`.
- Assigning a setting the current platform does not use does nothing; reading it returns its
  default.
- A boolean setting takes a boolean and a number setting takes a number; anything else throws a
  `TypeError`.
- Under stock React Native (`loom.renderer=fabric`) `Loom` still imports. `renderer` is `'fabric'`,
  the other settings report their defaults, and writes do nothing (with a development warning).

### `asyncText`

| | |
|---|---|
| Type, default | boolean, `true` |
| Platforms | iOS |
| Native config key | `asyncText` |
| Legacy override | `NSUserDefaults` `LoomAsyncText`, launch argument `-LoomAsyncText NO` |

Draw paragraph text on a background queue when it changes inside a box of the same size; see
[Asynchronous text on iOS](/guide/async-text). With it off, drawings run in `drawRect:` and the
text counters report `syncDisabled`. Read on every draw, so a change applies to the next one.

### `asyncTextFirstDraw`

| | |
|---|---|
| Type, default | boolean, `true` (compile-time default `LOOM_IOS_ASYNC_TEXT_FIRST_DRAW`) |
| Platforms | iOS |
| Native config key | `asyncTextFirstDraw` |
| Legacy override | `NSUserDefaults` `LoomAsyncTextFirstDraw` |

Draw a paragraph's first appearance in the background too, so its text appears a frame after the
view. Applies to views that draw for the first time after the write; set it at the top of
`index.js` to cover the first screen.

### `asyncTextResize`

| | |
|---|---|
| Type, default | boolean, `true` |
| Platforms | iOS |
| Native config key | `asyncTextResize` |
| Legacy override | `NSUserDefaults` `LoomAsyncTextResize` |

Redraw a one-off resize (a commit, not an animation) in the background. Applies to the next resize.

### `earlyCreate`

| | |
|---|---|
| Type, default | boolean, `true` |
| Platforms | iOS, Android |
| Native config key | `earlyCreate` (Android: `loom.earlyCreate`) |
| Legacy override | `NSUserDefaults` `LoomEarlyCreate`; `debug.loom.earlycreate` |

A commit that creates many views (a first screen, a navigation) sends their creates to the UI
thread as soon as it knows them, so views are built while the worker lays them out. Read per
commit; it only matters for large commits, so set it at the top of `index.js` to cover the first
screen.

### `parallelText`

| | |
|---|---|
| Type, default | boolean, `true` on iOS, `false` on Android |
| Platforms | iOS, Android |
| Native config key | `parallelText` (Android: `loom.parallelText`) |
| Legacy override | `NSUserDefaults` `LoomParallelText`; `debug.loom.paralleltext` |

A commit that creates many paragraphs measures their text on several cores before the real layout
pass. Off on Android: on a low-end phone the threads it used were the ones the UI thread needed.
Read per commit. On Android the measurement threads are started at startup when the resolved
value is `true`; turning it on later from JS starts them on first use.

### `jankReporter`

| | |
|---|---|
| Type, default | boolean, `true` |
| Platforms | iOS, Android (the reporter exists in debug builds only) |
| Native config key | `jankReporter` (Android: `loom.jankReporter`) |
| Legacy override | `NSUserDefaults` `LoomJankReporter`; `debug.loom.jank` (`0` turns it off) |

Report main-thread tasks over the budget. Read on every task, so it can be switched while running.

### `jankBudgetMs`

| | |
|---|---|
| Type, default | number, `50`; minimum `1` |
| Platforms | iOS, Android (debug builds) |
| Native config key | `jankBudgetMs` (Android: `loom.jankBudgetMs`) |
| Legacy override | `NSUserDefaults` `LoomJankBudgetMs`; `debug.loom.jank_budget_ms` |

Milliseconds a main-thread task may take before the jank reporter reports it. On Android the Java
half of the reporter is updated with each write.

### `profileWorker`

| | |
|---|---|
| Type, default | boolean, `false` (Android: `true` when built with `loom.profile=true`) |
| Platforms | iOS, Android |
| Native config key | `profileWorker` (Android: `loom.profileWorker`) |
| Legacy override | iOS environment `LOOM_PROFILE_WORKER=1`, `NSUserDefaults` `LoomProfileWorker`; Android build flag `loom.profile` |

Print worker and commit timing. Most of the timing code is compiled in only with
`loom.profile=true` on Android; the setting then switches its output.

### Read-only build info

| Name | Type | Meaning |
|---|---|---|
| `renderer` | `'loom'` or `'fabric'` | The renderer the app runs (`loom.renderer`) |
| `jsOnMainThread` | boolean | JavaScript runs on the thread that owns native views (`loom.js=main`, `RCT_LOOM_JS_IOS=main`) |
| `worker` | boolean | Layout, text and diffing run on a worker thread (`loom.worker`, `RCT_LOOM_WORKER_IOS`) |
| `version` | string | The `react-native-loom` package version |

## Debug-only settings

The jank reporter is part of the debug build only. Its settings are `jankReporter` and
`jankBudgetMs` above; `adb shell setprop debug.loom.jank_budget_ms N` and
`adb shell setprop debug.loom.jank 0` remain as overrides.
