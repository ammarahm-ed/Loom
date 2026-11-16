# Architecture overview

## From React to pixels

1. **React** renders and calls the renderer's host functions (`createNode`, `cloneNode…`,
   `appendChild`, `completeRoot`). Loom answers them with `LoomBinding` instead of Fabric's
   `UIManagerBinding`.
2. **LoomRenderer** keeps a mutable node tree. A commit reconciles the changed nodes, runs Yoga
   layout and text measurement, and produces one batch of view operations.
3. **The worker** (always part of the Loom renderer) does step 2 on its own thread. Small commits skip the
   worker and run inline, where handing off would cost more than the work.
4. **The view applier** applies the batch on the main thread in one call: create, update props,
   insert, remove, layout. On Android, props cross from C++ to Java packed into an int array,
   not as a map per view. Each view's `finalizeUpdates` runs once per commit, after all of its
   updates, as in Fabric.

Commits cost what changed: React's prop and text updates patch their nodes in place (no ancestor
clones), the tree tracks which nodes need preparing and layout, and only changed props are sent
to core views.

## Scheduling

- **Main-thread mode** runs JavaScript on Android's main looper. Discrete events (taps) are
  delivered to JavaScript during input handling; `requestAnimationFrame` callbacks run before the
  frame's traversal; non-urgent React work yields to frames.
- **Background mode** runs JavaScript on React Native's JS thread; events reach it without
  waiting for the next frame, and results are posted to the main thread as asynchronous
  messages so a pending frame cannot hold them back.

## Text on iOS

Paragraph text that changes inside a box of the same size is rasterized on a background queue
and swapped in when ready; see [Asynchronous text on iOS](/guide/async-text). Laid-out text is
cached and handed from measuring to drawing, so a paragraph is not laid out twice.

## Startup

A baseline profile for React Native's classes ships inside ReactAndroid, so store installs
compile the startup path ahead of time.

## Libraries

Libraries reach Loom through React Native's Fabric C++ contracts, which Loom presents on its own
tree, or through the native API in `<react/loom/Loom.h>`. See
[Supporting Loom in your library](/guide/supporting-loom).
