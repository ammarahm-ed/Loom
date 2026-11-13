---
aside: false
---

# What is Loom?

**Loom** is a renderer for React Native. It replaces **Fabric**'s shadow tree and mounting
pipeline with its own retained node tree, Yoga layout and mount batches, and it lets JavaScript
run on the main thread or on its own thread. React code, components and libraries are
unchanged.

Loom is not a fork. It is a package of overrides applied to the unmodified `react-native` from
npm (0.89.0-rc.0), so it tracks React Native releases closely.

## Where Loom sits

<LoomDiagram name="sits" />

A build switch chooses the renderer. With Loom switched off, the build is stock React Native.

## What Loom replaces and what it keeps

| | Fabric | Loom |
|---|---|---|
| Node tree | Immutable shadow tree, cloned on every commit | One retained, mutable tree; a commit updates the changed nodes in place |
| Layout | Yoga on the new shadow tree | Yoga on the retained tree, for the nodes that changed |
| From commit to views | Diff the old and new trees into mutations | One `LoomMountBatch` of typed view operations per commit |
| JavaScript thread | Its own thread | Main thread or its own thread (`loom.js`) |
| Layout thread | The committing thread | A worker thread, or the committing thread for small commits |

What stays: React and its host-function interface, Hermes, Yoga, `TextLayoutManager`, the
event dispatcher and the runtime scheduler, and the view managers and generated components
that libraries ship. Fabric's `UIManager` and `Scheduler` are constructed as they are without
Loom.

Libraries work unchanged. Those built on view managers and generated components need nothing.
Libraries that reach into Fabric's C++ contracts (shadow nodes, state, commit hooks) are served
by a compatibility layer that emulates those contracts and costs nothing for apps that do not
use them; see [Supported libraries](/guide/compatibility). An adapter exists only where a
library has no way to work without one: [Reanimated](/guide/reanimated)'s layout animations
and shared element transitions.

## How a frame is woven

Both renderers start from the same thing: React calling the renderer's host functions
(`createNode`, `cloneNode…`, `appendChild`, `completeRoot`) as it commits. What happens next
differs.

<LoomDiagram name="pipeline" />

- **Fabric** keeps an immutable shadow tree. Every commit clones the changed nodes and their
  ancestors, lays out the new tree, then diffs it against the previous one to find out what to
  mount.
- **Loom** keeps one mutable tree. A commit updates only the nodes React changed, lays them out,
  and emits the view operations directly, so there is no clone-and-diff step. A prop or text
  change updates its node in place, without cloning its ancestors, and only the props that
  changed are sent to the platform views, applied in a single call per commit.

## Threads

Loom runs JavaScript where you choose, on Android and iOS: on the **main thread**, like a
native app, or on its **own thread**, as React Native does by default. In either mode the
**worker** thread can do layout, text measuring and diffing.

**JavaScript on the main thread** (`loom.js=main`):

<LoomDiagram name="jsmain" />

**JavaScript on its own thread** (`loom.js=background`):

<LoomDiagram name="jsbg" />

Small commits skip the worker and run inline, because handing them off would cost more than
the work itself.

## A tap, in each mode

With JavaScript on the main thread, the tap is handled while Android is still processing the
touch, and the change is drawn in the very next frame:

<LoomDiagram name="tapmain" />

With JavaScript on its own thread, the event crosses to the JS thread and the result crosses
back, so the change usually lands one frame later:

<LoomDiagram name="tapbg" />

See [Choosing a mode](/guide/modes) for the full trade-off.

## What JavaScript on the main thread makes possible

Native code can call into React and get an answer before it returns. That enables things
React Native normally can't do:

- **Instant updates**: a `setState` in a touch handler is as fast as `setNativeProps`. See
  [Instant updates](/guide/instant-updates).
- **Frame-aligned work**: `requestAnimationFrame` callbacks run before the frame is drawn, and
  non-urgent React work yields to frames. See
  [Large updates and transitions](/guide/transitions).

## A platform-agnostic core

The renderer core contains no iOS or Android code. It computes layout and emits ordered mount
operations; a *host* turns them into native views and answers the few questions that need live
native state. The contract has four parts:

- **`LoomHost`**: an interface with two required methods, `uiExecutor` and `applyBatch`, plus
  optional per-node effects and live queries.
- **`LoomPlatformTraits`**: data the core reads once, such as component names, logging and
  worker thread setup.
- **Text services**: `LoomTextServices`, or the default services built on React Native's
  `TextLayoutManager`.
- **Frame hooks**: optional display-clock inputs that let React's time slicing and animations
  line up with the frame.

iOS and Android are hosts of this core. A third, headless C++ host with no views is the
reference implementation and runs the core's tests on a desktop. See
[Implementing a platform](/guide/platforms) for the contract.

## What stays the same

| | |
|---|---|
| Your React code | Unchanged |
| Components and APIs from `react-native` | Unchanged |
| Libraries built on view managers and generated components | Work as they are |
| Libraries using Fabric's C++ internals | Work unchanged through Loom's compatibility layer; see [Supported libraries](/guide/compatibility) |
| Hermes | Unchanged |
| Reanimated | Works unchanged; Loom builds a small adapter into the app ([Reanimated](/guide/reanimated)) |

## Glossary

| Term | Meaning |
|---|---|
| **Loom renderer** | Loom's replacement for Fabric's renderer: a mutable node tree, layout, and one batch of view operations per commit |
| **Worker** | A background thread that runs layout, text measuring and diffing for large commits |
| **Host** | A platform's implementation of `LoomHost`: iOS, Android or the headless reference |
| **Mount batch** | `LoomMountBatch`: the ordered view operations of one commit, applied by the host in one call |
| **Inline commit** | A small commit processed on the current thread instead of the worker |
| **Main-thread mode** | `loom.js=main` (`RCT_LOOM_JS_IOS=main` on iOS): JavaScript runs on the main thread |
| **Background mode** | `loom.js=background` (`RCT_LOOM_JS_IOS=background` on iOS): JavaScript runs on its own thread |
| **Overrides** | The files Loom adds to or changes in `react-native`, listed in `overrides.json` |
| **Materialized tree** | `node_modules/.loom/react-native`: the installed `react-native` with the overrides applied |

## Platforms

| | Android | iOS |
|---|---|---|
| Loom renderer | Yes | Yes |
| Worker | Yes | Yes |
| JavaScript on the main thread | Yes (`loom.js=main`) | Yes (`RCT_LOOM_JS_IOS=main`) |
| JavaScript on its own thread with the Loom renderer | Yes (`loom.js=background`) | Yes (`RCT_LOOM_JS_IOS=background`) |
| Text drawn off the main thread | No | Yes ([asynchronous text](/guide/async-text)) |
| Startup baseline profile | Yes | Not applicable |
| Debug jank reporter | Yes | No |
