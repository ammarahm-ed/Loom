---
aside: false
---

# Implementing a platform

Writing a library that supports Loom is a different job: see
[Supporting Loom in your library](/guide/supporting-loom).

The renderer core has no iOS or Android code. It computes layout and emits ordered mount
operations; a *host* turns them into native views and answers the few questions that need live
native state. This page is the contract for writing one. iOS and Android implement it today, and
`hosts/headless` implements it with no views at all.

## What a host provides

| Piece | Header | Role |
| --- | --- | --- |
| `LoomHost` | `loom/host/LoomHost.h` | Mounting, per-node effects, live queries, the UI executor. |
| `LoomTextServices` | `loom/text/LoomTextServices.h` | Text measurement and its optional capabilities. |
| `LoomPlatformTraits` | `loom/host/LoomPlatformTraits.h` | Data the core reads once: component names, policies, logging, thread setup. |
| Frame inputs | `runtimescheduler/RuntimeSchedulerFrameCallbackScope.h` | Optional display-clock feeds. |

Fabric's `UIManager` and `Scheduler` are constructed as without Loom and carry no Loom methods.

## Architecture

How a commit reaches native views. A host supplies the `LoomHost` box, the native events and
frame hooks on top, and the text services and traits at the bottom; everything else is the core.

<LoomDiagram name="architecture" />

With `loom.js=main` the JS thread is the UI thread, so the first two cards of the JS lane run
on the UI thread. `FrameClock::onFrame` and `FrameCallbackScope` feed the runtime scheduler,
`flushAnimationFrames` feeds `LoomBinding`, and `setSurfaceConstraints`, `setPointScaleFactor`
and `releaseSurface` are static calls on `LoomBinding`.



## Registering the host

Register one host in the UI manager's `ContextContainer` before the JS runtime installs the
binding:

```cpp
LoomHost::registerIn(*contextContainer, std::make_shared<MyHost>(...));
```

`registerIn` stores the host under `LoomHost::kContextKey` and installs `host->traits()`
process-wide. The binding reads the host and its text services once, when it is created. Creating
a binding with no host is a fatal error that names `registerIn`.

The container owns the host for the life of the instance. A host that refers to platform objects
holds them weakly or invalidates itself on teardown, and treats calls after teardown as no-ops.

## Threads

- **UI thread:** the one thread that owns native views and applies results. `uiExecutor()` posts
  to it.
- **Commit thread:** the thread a commit is computed on: the JS thread, or the Loom worker when it
  is enabled.

Any thread, and never blocking on the UI thread: `uiExecutor()` and the text services. Text is
measured on the commit thread only when `LoomTextServices::canMeasureOnWorker()` is true;
otherwise a commit that contains text is computed on the UI thread. Every other `LoomHost` method
is called on the UI thread. With the worker, effects recorded during a commit are replayed in
order on the UI thread together with that commit's result.

### One commit

The same flow as a sequence, with the thread of each step. With `loom.js=main` the JS thread
is the UI thread; otherwise they differ and the UI thread never waits for JavaScript.

<LoomDiagram name="commit" />

Where the compute step runs: on the worker thread by default; on the calling thread when the
commit is small and the worker is idle (on the UI thread the result is then applied before the
call returns); on the UI thread for a synchronous commit, or when text cannot be measured off the
UI thread. In every case the effects are replayed on the UI thread, in order. A commit with many
new nodes may first deliver an `applyBatch` that carries only their creates, while the rest is
still being computed.

## `LoomHost`

Only `uiExecutor` and `applyBatch` are required; everything else defaults to doing nothing.

| Method | Called | Meaning |
| --- | --- | --- |
| `uiExecutor()` | any thread | Posts a function to the UI thread, FIFO, never inline on the caller's stack. An empty function means there is no separate UI thread. |
| `applyBatch(surface, batch)` | UI | Applies one commit. The callee owns the batch. See below. |
| `failSurface(surface, reason)` | UI | A commit failed part-way and the core stopped the surface. |
| `updateViewProps(tag, props)` | UI | Applies flat changed props to a mounted view outside a batch: `setNativeProps` and animated paint. |
| `registerEventTarget`, `unregisterEventTarget` | UI | The JS instance handle for a node; retain it until unregistered. |
| `dispatchEvent(...)` | UI | An event the core raised for a tag (layout events and similar); forward it to React's event dispatcher. |
| `updateComponentState(surface, tag, state)` | UI | A component's own `State`, as Fabric's UpdateState mutation. |
| `updateTextState(update)` | UI | The resolved text payload of a paragraph or text input (`LoomTextStateUpdate`). |
| `updateScrollContentBounds(...)` | UI | A scroll view's content bounds and props, in DIP. |
| `dispatchCommand`, `sendAccessibilityEvent`, `setIsJSResponder` | UI | `UIManager` operations addressed by tag. |
| `reportUnsupportedInlineAttachment` | UI | A native view was found inside a Text. The core then fails the surface. |
| `getScrollOffset(surface, tag)` | UI | Live scroll position in DIP. Default `{0, 0}`. |
| `getViewLocationInSurface(surface, tag)` | UI | A mounted view's origin relative to its surface root, or `nullopt`. |
| `textServices()` | once | The text engine, or null (text then uses an estimate). |
| `traits()` | once, in `registerIn` | See below. |

## The batch format

`applyBatch` receives a `LoomMountBatch`: a vector of typed operations plus, in
`phaseOperationIndexes`, the same operations grouped by phase, each phase in emission order. Apply
the phases in this order:

1. **CreateAndProps.** `DirectCreateOperation` (tag, layoutable, componentName, props, optional
   `parsedProps`) and `DirectUpdatePropsOperation` (tag, props, optional `parsedProps`). `props` is
   the whole prop set on create and the changed keys on update, with resets as explicit nulls;
   which components receive diffs is a trait. `parsedProps` is set only for components with their
   own `ComponentDescriptor`; a host that builds `Props` objects uses it instead of re-parsing.
2. **Removes.** `DirectRemoveOperation` (parentTag, tag, index): detach, but keep the view alive
   until a delete.
3. **Inserts.** `DirectInsertOperation` (parentTag, tag, index).
4. **Padding.** `DirectPaddingOperation` and `DirectOverflowInsetOperation` (tag, insets as
   left, top, right, bottom in DIP).
5. **Layout.** `DirectLayoutOperation` (tag, parentTag, displayType, layoutDirection, x, y, width,
   height): the frame in DIP relative to the parent.
6. **Deletes.** `DirectDeleteOperation` (tag): destroy the view.

Other fields: `instructionCount` and `objectCount` size a host's own encoding;
`requiresTagReplacement` says a tag is deleted and created in the same batch, so a host that keys
views by tag must apply the delete first; `animationFrame` marks a batch produced by an
animation lane rather than a React commit, whose resized text must be drawn in the same frame.
The `componentName` in a create is the node's *host name* (see the component table).

## Traits

`LoomPlatformTraits` is filled once and read through `LoomPlatformTraits::current()`. Every field is
data or a function pointer, so a read is a load. The defaults are the generic host.

| Field | Meaning |
| --- | --- |
| `components` | The component vocabulary: roles (box, paragraph, nested text, raw text, generic state wrapper, intrinsic measurement, core-rendered), React-to-host names, `getTagName` names, the host name of text leaves. Default: Fabric names with identity host names. `loomViewManagerComponentTable()` is the ViewManager-style vocabulary iOS and Android share. |
| `sendsPropDiffs(hostName)` | Whether updates of that component carry only changed keys. Null: never. |
| `republishTextStateOnResize` | Text is drawn in the state's content size, so a resize republishes it. |
| `hasPreparedTextLayout` | `prepareTextForMount` returns a layout the host draws from. Without it the core publishes the content box with the text state. |
| `parallelFor` | Runs `body(0..n-1)`, possibly in parallel. Null: no speculative text measurement. |
| `log` | Receives every formatted log and profile line. Null: stderr. |
| `profileWorker` | Enables profile lines and timing. |
| `isUIThread` | True on the thread that owns views. Null: always true. |
| `workerStackSize`, `workerThreadName`, `setCurrentThreadName` | Worker thread setup. Zero stack: the platform default. |
| `withWorkerScope` | Wraps each worker job, for hosts that must attach the thread to a managed runtime. |
| `runtime` | Hooks for the runtime-scheduler layer: `readIntSetting(name, fallback)` and `postDelayed(task, delayNanos)`. |

Install the traits before the first surface starts; they are process-wide.

## Text

`LoomHost::textServices()` returns a `LoomTextServices`. Only `measureText` is required.

| Method | Default |
| --- | --- |
| `measureText(surface, string, paragraphAttributes, min, max)` | required; `nullopt` falls back to an estimate |
| `canMeasureOnWorker()` | false |
| `parseTextAttributes(...)` | returns the inherited attributes unchanged |
| `prepareText`, `prepareTextForMount` | invalid (no prepared layout) |

A host that only has React Native's `TextLayoutManager` uses `LoomTextLayoutManagerServices`
(`loom/text/LoomTextLayoutManagerServices.h`, included by hosts, never by the core):

- `measureText` is `TextLayoutManager::measure`, with the point scale factor the host passes to
  `setPointScaleFactor`.
- `parseTextAttributes` is React Native's own props classes. The React name goes through
  `componentNameByReactViewName`; `Paragraph` is parsed as `ParagraphProps` and nested `Text` as
  `TextProps`, whose `BaseTextProps` conversion produces the `TextAttributes`. This is what the
  shadow-tree path builds. Other components carry no attributes.
- Off-UI measurement is a constructor option.

Subclass it to keep the default parser and override only what the platform does differently: iOS
sets the font-size multiplier before parsing, and both iOS and Android override measurement and
prepared layout.

## Input from the host

Static entry points on `LoomBinding`, called by the host:

- `setSurfaceConstraints`, `setPointScaleFactor`: viewport and density.
- `releaseSurface`: the host dropped a surface.
- `recordNativeProps`, `recordNativePaintProps`, `recordAnimationProps`: native and animation
  backend prop writes.
- `dispatchComponentState`: a native state update for a node.

Native events go through `LoomEventManager`.

## Frame hooks

All optional. Without them the scheduler uses fixed time slices and commits land as they complete.

- **`FrameClock::onFrame(frameTimeNanos)`**, once per display frame, with the frame's start time on
  the `steady_clock` epoch. Lets React's time slicing leave room for the frame.
- **`FrameCallbackScope`**, constructed on the UI thread around animation-phase callbacks
  (requestAnimationFrame and timer delivery) when JS shares that thread (`loom.js=main`). On
  exit it runs the event loop that was requested inside it and applies the commits before the frame
  is drawn.
- **`LoomBinding::flushAnimationFrames()`**, the before-frame hook, called on the UI thread just
  before the frame's mount dispatch. The runtime scheduler also calls it at the end of every JS
  task.
- In return the scheduler may call `runtime.postDelayed`.

What each host drives today:

| Host | `FrameClock` | `FrameCallbackScope` | Before-frame hook |
| --- | --- | --- | --- |
| Android | Choreographer frame callback | Choreographer animation phase | Frame callback, before mount dispatch |
| iOS | `CADisplayLink` | none | end-of-task listener only |
| headless | none | none | end-of-task listener only |

## Build switches

`LOOM_RENDERER`, `LOOM_WORKER` and `LOOM_PROFILE` select the renderer, the worker
thread, whether JS runs on the UI thread, and profiling. `RN_JANK_REPORTER=1` turns on the debug
main-thread jank reporter; its settings come from `runtime.readIntSetting`.

## Starting from the headless host

`hosts/headless` is a complete host with no platform code, built on the core's generic defaults
(`platform/cxx`):

- `LoomHeadlessHost` records every batch, event, command, prop write and text state, readable with
  `takeBatches()` and the other `take…()` calls.
- `LoomHeadlessTextServices` measures deterministically: a fixed width per character and height per
  line, wrapping inside the maximum width, with `maximumNumberOfLines` honored.
- `uiExecutor()` runs work inline (`Executor::Inline`) or queues it until `drainUIQueue()`
  (`Executor::Queued`). Use `Queued` when the worker is on, so results are applied on the thread
  that owns the host.

To start a new platform, copy it and replace one thing at a time:

1. Keep the default traits and Fabric component names. Replace `applyBatch` with code that applies
   the six phases to your view tree.
2. Return a real `uiExecutor()` and set `traits.isUIThread`.
3. Return `LoomTextLayoutManagerServices` once your platform has a `TextLayoutManager`.
4. Fill the remaining effects, then the live queries.
5. Add a traits fill only for what differs: a ViewManager-style vocabulary, prop diffs, logging,
   worker thread setup.
6. Feed `FrameClock` and the before-frame hook if you have a display clock.

### Building the core on a desktop

```sh
cmake -S packages/react-native-loom/hosts/headless -B build/loom-core \
      -DLOOM_REACT_NATIVE_DIR=<app>/node_modules/.loom/react-native
cmake --build build/loom-core --target loom-core-headless-tests loom-core-tests
ctest --test-dir build/loom-core
```

`LOOM_REACT_NATIVE_DIR` is a tree produced by `loom materialize`; re-run it after changing
`packages/react-native-loom/src`. React Native's own CMakeLists build the `ReactCommon` libraries,
selecting `platform/cxx` when `ANDROID` is not set. folly, glog, fmt, double-conversion, fast_float
and Boost are fetched at the versions the materialized tree pins, with React Native's third-party
CMakeLists (`-DLOOM_USE_SYSTEM_DEPS=ON` uses system packages instead). `loom-core-tests` runs the
renderer and worker suites; `loom-core-headless-tests` runs the host and a binding test that
commits through a registered host with no `UIManagerDelegate`.
