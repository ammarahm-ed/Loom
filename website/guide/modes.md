# Choosing a mode

Loom can run JavaScript in two places. Both use the Loom renderer; they trade different things.

| | JavaScript on the main thread (`loom.js=main`) | JavaScript on its own thread (`loom.js=background`) |
|---|---|---|
| Tap to visible change | Same frame, like a native app (about 14-16 ms on a 60 Hz phone) | One frame later (about 18-23 ms) |
| `setState` vs `setNativeProps` | Same speed | `setNativeProps` is faster |
| Heavy JavaScript (long handlers, big `JSON.parse`) | Blocks frames, like heavy work on a native app's main thread | Never blocks frames |
| Large splittable updates (`startTransition`) | Split across frames | Run beside frames |
| Large urgent updates | Block frames until done | Run beside frames |

## Pick main-thread mode when

- Your app's work per interaction is small, and you want native-feeling input and lists.
- You want native components that call into React synchronously.

Keep heavy work off the main thread: use `startTransition` for large non-urgent renders, split
long tasks, or move them to a worker. The debug-time [jank reporter](/guide/jank-reporter)
tells you where to look.

## Pick background mode when

- Your app runs long synchronous JavaScript that you can't split.
- You rely on libraries that do heavy JavaScript work during interactions.

## Switching

Modes are compile-time switches (`loom.js` on Android, `RCT_LOOM_JS_IOS` on iOS); see
[Configuration](/reference/configuration). The same JavaScript runs in both modes.

Measured on a low-end Android phone and on an iPhone, JavaScript on its own thread is the
faster mode for large updates and keeps scrolling smooth; JavaScript on the main thread gives
the fastest taps. See [Benchmarks](/benchmarks).
