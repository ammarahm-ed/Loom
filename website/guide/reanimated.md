---
aside: false
---

# Reanimated

`react-native-reanimated` works on Loom with no changes to the app or to Reanimated: animated
styles and shared values, `measure` and `scrollTo`, layout animations (`entering`, `exiting`,
`layout`), and shared element transitions.

| | Supported |
|---|---|
| react-native-reanimated | 4.7.1 |
| react-native-worklets | 0.13.x |

## Animations

Reanimated writes animated props through React Native's shared animation backend, which Loom
implements on its own renderer: a paint-only prop (`opacity`, `transform`, colors) goes straight
to the view, and a layout prop (`width`, `top`, ...) is laid out by Loom and mounted in the same
frame. Nothing is committed to a tree for an animation frame.

<LoomDiagram name="animprops" />

Until the frame is folded into the renderer's props, the lane keeps the paint values as a
replayable overlay. Writes to a text node leave the lane and take the ordinary per-call path.

Reanimated chooses that path at build time through two static feature flags. Loom's build
integration sets them for Reanimated's native build on both platforms, whatever the app
configures:

| Flag | Value on Loom |
|---|---|
| `USE_ANIMATION_BACKEND` | `true` |
| `FORCE_REACT_RENDER_FOR_SETTLED_ANIMATIONS` | `false` |

Other static flags are the app's own (`reanimated.staticFeatureFlags` in `package.json`).

`measure`, `scrollTo` and reading a view's layout from a worklet read Loom's tree through the
same `UIManager` calls Reanimated makes on Fabric.

## Layout animations

On Fabric, Reanimated runs layout animations by rewriting every mount transaction of a surface,
and to do that it keeps its own copy of every view on the surface. Loom has no shadow tree and
no mutation stream for it to rewrite, and producing them for Reanimated would add their cost to
every commit of every app that installs Reanimated.

Instead, Loom builds a small adapter into the app when a supported Reanimated is installed
(`packages/react-native-loom/adapters/reanimated`). It receives each of Loom's mount batches
before the platform applies it and follows only the views of Animated components; every other
view costs one lookup per mount operation that names it. The animations themselves are still
Reanimated's own: the adapter starts them through Reanimated's code, which runs the configured
worklet on the UI runtime, and mounts each frame's values.

<LoomDiagram name="layoutanim" />

Exits follow Reanimated's rule. An exiting view keeps its subtree mounted. A removed ancestor of
exiting views stays until the last of them ends, and its other children are removed at once.
Siblings inserted or removed while views exit keep their order.

The adapter is automatic:

- **iOS:** the `RNReanimated` pod leaves out one source file (`ReanimatedModuleProxy.cpp`), and
  the autolinked `RNLoomReanimated` pod compiles the adapter and the replacement for that file.
- **Android:** Reanimated's own CMake build runs unchanged; the adapter's sources take that
  file's place in `libreanimated`.

`node_modules` is never modified. The replacement file is Reanimated's own with a four-line
change: the layout-animations handler is created by the adapter. Another Reanimated version
builds unmodified, with a build warning; its layout animations mount without animating.

## Shared element transitions

Shared element transitions are behind Reanimated's experimental static flag. Enable it in the
app's `package.json`:

```json
{
  "reanimated": {
    "staticFeatureFlags": {
      "ENABLE_SHARED_ELEMENT_TRANSITIONS": true
    }
  }
}
```

and use them as on Fabric: a `sharedTransitionTag` (and optionally a `sharedTransitionStyle`)
on Animated components, and each screen's content inside a `SharedTransitionBoundary`:

```tsx
import Animated, {SharedTransitionBoundary} from 'react-native-reanimated';

function Detail() {
  return (
    <SharedTransitionBoundary isActive>
      <Animated.Image sharedTransitionTag="photo" source={source} style={styles.photo} />
    </SharedTransitionBoundary>
  );
}
```

How they run on Loom:

- The screen on top is the newest active boundary. When a commit changes it, every shared view
  of the previous boundary whose tag is also in the new one is hidden together with its
  counterpart, and a copy of it, mounted at the root of the surface, animates from one to the
  other with Reanimated's `SharedTransition`. The target appears when the copy is removed.
- Frames are measured where the views really are. The source is measured on its mounted native
  view, so scroll offsets, native headers and a screen that the same commit removes are all
  accounted for. The target comes from Loom's layout, corrected by the offset its view had the
  last time it was on screen (a screen that comes back is still scrolled as it was). A pushed
  screen that React Native Screens moves below its header after mounting retargets the copy.
- On iOS, a pop that React Native Screens performs natively (the back button, the back swipe)
  is driven by the native transition's progress, as in Reanimated: the copy follows the
  gesture, and a cancelled gesture restores both screens. A pop started from JavaScript
  (`navigation.goBack()`), and every pop on Android, starts from the commit that removes the
  screen.

<LoomDiagram name="shared" />

`SharedTransitionBoundary` is `display: contents`, so, as on Fabric, it mounts no view of its
own and does not affect touch handling or layout.

Without the flag, none of the shared transition code is compiled in.

## What would make the adapter unnecessary

One public extension point in Reanimated: a way for the renderer to supply the per-surface
layout-animations handler, for example a factory registered through React Native's
`ContextContainer` that Reanimated uses instead of its default. The adapter's handler would then
ship as ordinary Loom code against Reanimated's public headers, and no Reanimated source file
would be replaced.
