# Loom adapter for react-native-reanimated

Makes Reanimated's layout animations (`entering`, `exiting`, `layout`) and shared element transitions work on Loom's renderer, at a cost confined to the views that can animate. Loom builds it into the app automatically when a supported Reanimated version is installed; there is nothing to install or configure.

Supported: `react-native-reanimated` 4.7.1. Any other version builds unmodified, with layout animations mounting without animating, and a build warning.

## Why it exists

Reanimated runs layout animations through `LayoutAnimationsProxy`, a `MountingOverrideDelegate` that rewrites every Fabric mount transaction of a surface. To decide what an `exiting` subtree keeps mounted, where a re-parented view moves, and which screen a view belongs to, it keeps its own copy of every view on the surface (its "light tree"), fed by the full mutation stream from the surface's first commit. That design needs every view of every commit, with parsed props and layout, whether or not anything on the screen animates.

Loom has no shadow tree and no mutation stream. Producing one for Reanimated would add that cost to every commit of every app that installs Reanimated, which Loom does not accept. Loom's renderer already knows every view's parent, props and frame on demand, so the copy is not needed: the adapter follows only the views of Animated components, and starts Reanimated's animations itself when one of them is inserted, re-laid out or removed.

Reanimated has no public way for a renderer to provide that per-surface handler. The proxies are created by `ReanimatedModuleProxy` from a factory it builds privately, and the animation frames and ends are routed to them through a private registry. So the adapter replaces one Reanimated source file.

## What it changes

- `patched/<version>/ReanimatedModuleProxy.cpp`: Reanimated's file with one change: the layout-animations proxy registry is created by `reanimated::loom::createLayoutAnimationsProxyRegistry` (4 lines, see `git diff --no-index` against the release).
- `cpp/LoomLayoutAnimationsProxy.*`: a `LayoutAnimationsProxyCommon` subclass, so Reanimated's own code still starts animations, collects their frames and ends them. It replaces the light tree with Loom's batches (`LoomLayoutAnimationDriver`, in Loom's renderer) and mounts each frame as Loom operations.

`node_modules` is never modified:

- iOS (`integration/ios/loom_pods.rb`): the `RNReanimated` pod excludes its `ReanimatedModuleProxy.cpp`, and the `RNLoomReanimated` pod (autolinked from `react-native-loom`, `RNLoomReanimated.podspec` in its root) compiles the replacement and the adapter.
- Android (`integration/android/loom.settings.gradle` and `android/loom-reanimated.cmake`): Reanimated's own CMake build runs unchanged, and after it defines `libreanimated` the replacement and the adapter take the original file's place in it.

Exits follow Reanimated's own rule: an exiting view keeps its subtree mounted; a removed ancestor of exiting views stays until the last of them ends, and its other children are removed at once. While views exit, insertions and removals in their live parent are moved past them, so siblings keep their order.

## Shared element transitions

With Reanimated's `ENABLE_SHARED_ELEMENT_TRANSITIONS` static flag set in the app's `package.json`, the adapter runs Reanimated's shared element transitions on the same model as Reanimated's proxy. Without the flag the code is compiled out.

- Screens (React Native Screens' `RNSScreen`, `RNSModalScreen`) are followed from the batches. A `REASharedTransitionBoundary` is `display: contents`, so, as in Fabric, it mounts no view and is never in a batch: an Animated view's nearest boundary is found in the renderer when the view is inserted (walking from its parent in React's tree, since the host parent skips flattened views), and each known boundary's `isActive` and removal are read from the renderer with every batch the adapter processes.
- The screen on top is the newest active boundary. When a commit changes it, each shared view of the old boundary whose `sharedTransitionTag` is also in the new one gets a copy (a "container": the source's component and props, created and inserted at the surface root). Reanimated's `SharedTransition` animates the copy from the source's frame to the target's; both views are hidden meanwhile, and the target shows when the copy is removed.
- Frames are surface frames. The source is measured on its mounted native view before the batch mounts, so scroll offsets, native headers and a screen that the batch removes are accounted for. The target comes from the renderer's layout, plus the offset its view had from its layout the last time it was measured on screen (a screen that comes back is still scrolled as it was), and a screen with a header is moved below it by the height its stack has over it (Reanimated's rule).
- A pushed screen gets its frame below the header from React Native Screens only after it mounts. While a copy flies every batch is checked, and a copy whose target moved is restarted toward it, as Reanimated retargets.
- iOS: a pop React Native Screens performs natively (back button, swipe) is driven by its transition progress, as in Reanimated: the copy follows the progress, a cancelled gesture restores both screens, and the popped screen's views stay hidden until React removes it. A pop React starts (`goBack()` from JS) and Android's pops start from the commit that removes the screen, as in Reanimated.
- Loom side: `LoomLayoutAnimationGeometry` gives the driver renderer frames, parents, component names and props while it processes a batch, and native view origins (`loomGetViewLocationInSurface`) on the UI thread.

## React Native 0.89

Reanimated 4.7.x supports React Native 0.86 to 0.88. On 0.89 its Android CMake build lists React Native's `graphics/platform/cxx` headers ahead of the Android ones libreactnative is built with, and the two define `Color` differently (`int32_t` against `{int32_t, bool}`). Every props field after the first color then sits at another offset in libreanimated than in libreactnative. Layout animations read only `opacity` and are unaffected; a shared transition's props diff (`PropsDiffer`) reads `boxShadow` and `borderRadii` from the wrong place and can crash. `android/loom-reanimated.cmake` replaces that include directory with `graphics/platform/android`; it goes away once Reanimated builds against 0.89's headers.

## What upstream would need to remove it

One public extension point in Reanimated: a way for the host renderer to supply the per-surface layout-animations handler, for example a `LayoutAnimationsProxyFactory` the renderer registers (through the `ContextContainer`, as React Native components already receive renderer services) that `ReanimatedModuleProxy::initializeLayoutAnimationsProxyRegistry` uses instead of its default. With that, this adapter's proxy ships as ordinary Loom code against Reanimated's public headers, and `ReanimatedModuleProxy.cpp` is no longer replaced.

Reanimated's static feature flags (`USE_ANIMATION_BACKEND`) are set by Loom's integration separately and do not depend on this adapter.
