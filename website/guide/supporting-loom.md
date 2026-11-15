# Supporting Loom in your library

Most libraries work on Loom unchanged ([Supported libraries](/guide/compatibility)). A library that
reaches into the renderer, to animate props from native code, read layout or react to a commit,
can do better than the compatibility layer: with `LOOM_RENDERER` defined it talks to Loom directly
through `<react/loom/Loom.h>`, and the same codebase keeps its Fabric path for stock React Native.

This guide walks through `examples/loom-example-library`, a small library that supports both
renderers on Android and iOS from one C++ file. It is linked into `apps/loom-test-app` and shown
on the `example` screen.

## What the example does

One C++ TurboModule, `LoomExample`, with four methods:

| Method | What it asks of the renderer |
|---|---|
| `renderer()` | which renderer answered: `'loom'` or `'fabric'` |
| `measure(tag)` | the node's frame in its surface, from the renderer's layout |
| `setOpacity(tag, opacity)` | write a prop of a node from native code |
| `frames()` | how many frames (Loom) or mounts (Fabric) the renderer reported |

## 1. Detect Loom

Stock React Native has no Loom header, so test for it first:

```cpp
#if __has_include(<react/renderer/LoomConfig.h>)
#include <react/renderer/LoomConfig.h>
#endif

#if defined(LOOM_RENDERER) && LOOM_RENDERER
#include <react/loom/Loom.h>
static_assert(LOOM_API_VERSION >= 1, "needs Loom API 1");
#else
// Fabric
#endif
```

- `LOOM_RENDERER` is `1` only in Loom builds. A Loom build with `loom.renderer=false` defines it as
  `0`; stock React Native defines nothing and has no `LoomConfig.h`, so the Fabric branch is taken.
- `LOOM_API_VERSION` is an integer that only grows, by additions. A library compiles against the
  version it needs and checks `LOOM_API_VERSION >= n`.
- The header is the same in the prebuilt and in a source build. Android publishes it through the
  `ReactAndroid::reactnative` prefab module (`include/react/renderer/LoomConfig.h` and
  `include/react/loom/Loom.h`); iOS ships both in the headers xcframework and in the `React-Fabric`
  pod.

## 2. Build setup

Nothing Loom-specific is needed beyond what a library using React Native's C++ already has.

**Android.** Link the prefab modules as for any library:

```cmake
find_package(ReactAndroid REQUIRED CONFIG)
find_package(fbjni REQUIRED CONFIG)
add_library(loomexample SHARED OnLoad.cpp ../../../../cpp/LoomExample.cpp)
target_link_libraries(loomexample ReactAndroid::reactnative ReactAndroid::jsi fbjni::fbjni android log)
```

Enable `prefab true` in `build.gradle`. The app resolves `com.facebook.react:react-android` to
Loom's artifact (`loom.settings.gradle`), so the same `find_package` finds Loom's headers.

**iOS.** `install_modules_dependencies(s)` in the podspec is enough: the React pods that carry the
headers are Loom's when the app uses Loom.

**One module, both platforms.** The example registers its C++ TurboModule in React Native's global
C++ module map, which both platforms' TurboModule managers consult: from `JNI_OnLoad` on Android
(`LoomExamplePackage` only loads the library) and from `+load` on iOS
(`ios/LoomExampleRegistration.mm`).

## 3. The `#if LOOM_RENDERER` split

The two backends have the same shape, so the rest of the module does not know which one it runs.

::: code-group

```cpp [Loom]
class Backend {
 public:
  explicit Backend(loom::Runtime runtime) : runtime_(std::move(runtime)) {
    // runs on the UI thread right after a frame's batch was applied
    subscription_ = runtime_.onFrameApplied(
        [this](const loom::FrameInfo&) { frames_.fetch_add(1); });
  }

  static std::unique_ptr<Backend> create(jsi::Runtime& rt) {
    auto runtime = loom::Runtime::from(rt);
    return runtime ? std::make_unique<Backend>(*runtime) : nullptr;
  }

  // the frame in the surface, from Loom's layout
  std::optional<Rect> measure(Tag tag) const {
    if (auto surfaceId = surfaceOf(tag)) {
      if (auto metrics = runtime_.layoutRelativeTo(*surfaceId, tag, std::nullopt)) {
        return metrics->frame;
      }
    }
    return std::nullopt;
  }

  // the animation lane: a typed prop bag, no clone, no commit, no JS
  bool setOpacity(Tag tag, double opacity) const {
    auto surfaceId = surfaceOf(tag);
    if (!surfaceId) return false;
    loom::AnimatedPropBag bag;
    bag.props.push_back(std::make_unique<AnimatedProp<Float>>(OPACITY, opacity));
    runtime_.updateAnimatedProps(*surfaceId, tag, std::move(bag));
    return true;
  }
  // ...
};
```

```cpp [Fabric]
class Backend : public UIManagerMountHook {
 public:
  explicit Backend(UIManager& uiManager) : uiManager_(&uiManager) {
    uiManager_->registerMountHook(*this);
  }

  static std::unique_ptr<Backend> create(jsi::Runtime& rt) {
    auto binding = UIManagerBinding::getBinding(rt);
    return binding ? std::make_unique<Backend>(binding->getUIManager()) : nullptr;
  }

  void shadowTreeDidMount(const RootShadowNode::Shared&, HighResTimeStamp) noexcept override {
    frames_.fetch_add(1);
  }

  // walk the shadow tree of every surface
  std::optional<Rect> measure(Tag tag) const {
    std::optional<Rect> frame;
    uiManager_->getShadowTreeRegistry().enumerate([&](const ShadowTree& tree, bool& stop) {
      auto root = tree.getCurrentRevision().rootShadowNode;
      if (auto node = find(*root, tag)) {
        frame = LayoutableShadowNode::computeRelativeLayoutMetrics(
                    node->getFamily(), *root, {}).frame;
        stop = true;
      }
    });
    return frame;
  }

  // clone the node with new props and commit the tree
  bool setOpacity(Tag tag, double opacity) const { /* ShadowTree::commit with cloneTree */ }
  // ...
};
```

:::

Everything else, the TurboModule, its methods and the platform glue, is shared. See
`examples/loom-example-library/cpp/LoomExample.cpp`.

## 4. How Loom differs from Fabric

| Concept | Loom | Fabric |
|---|---|---|
| Surface | one root; owns a node tree, a Yoga tree and a mount queue | `ShadowTree` / `SurfaceHandler` |
| Node | retained and mutable, identified by `Tag`; props are a merged dictionary | a new immutable `ShadowNode` per commit |
| Frame | a surface's layout result, applied to views as one batch on the UI thread | a `MountingTransaction` |
| Prop write | write to the node by `Tag`; layout runs only if a prop needs it | clone and commit |

Loom does not clone a tree to change a prop. A library holding a `Tag` writes the prop to the node.

The API (all in `facebook::react::loom`, value types are React Native's own):

- **Entry:** `Runtime::from(jsi::Runtime&)` on the JS thread, or `Runtime::from(const ContextContainer&)`
  anywhere you have the container. Both return `std::nullopt` when the app does not run Loom.
- **Surfaces and nodes:** `forEachSurface`, `surfaceSize`, `hasNode`, `props`, `layout`,
  `layoutRelativeTo`, `componentName`, `children`, `parent`.
- **Prop writes, three lanes:** `updateProps` (the JS lane, like a React commit),
  `updateAnimatedProps` (the animation lane, a typed `AnimatedPropBag`, coalesced per frame; a layout
  prop makes Loom lay the node out), `updateNativeProps` (applied to the view and remembered so the
  next commit does not undo it).
- **Frames:** `onFrameApplied` (UI thread), `onLayoutCommitted` (commit thread), `beforeNextFrame`.
  Subscriptions are RAII: destroying the handle unsubscribes.
- **Native state:** `updateState` for components with their own descriptor and state.
- **Settings:** `setting(LoomSettingId)`.

## 5. Threading rules

- **JS thread:** `Runtime::from(jsi::Runtime&)`.
- **Any thread:** reads, `updateProps`, `updateAnimatedProps`, `updateNativeProps`, subscriptions. A
  read from the JS or commit thread first waits for the commits already submitted; a read on the UI
  thread does not block.
- **UI thread:** `onFrameApplied` and `beforeNextFrame` callbacks.
- **Commit thread** (the JS thread, or the Loom worker when it runs): `onLayoutCommitted` callbacks.
  Do not touch views there; hop to the UI thread.
- With `loom.js=main` the JS thread is the UI thread. The rules above still hold and callbacks may
  simply run on the same thread. Do not assume they differ.

## 6. When a library does not adopt the API

A library that still uses Fabric's C++ contracts works: under Loom they are rerouted to Loom's own
code, or are safe no-ops. This is the same in every flavor (debug, release, profiling).

| Fabric contract | Under Loom |
|---|---|
| `ShadowTree::commit` on nodes the library holds | rerouted: the props the commit changes are applied to Loom's nodes; other commits run the transaction on the surface's empty root and its commit hooks |
| `UIManager::registerCommitHook` / `registerMountHook` | kept: commit hooks run on those commits, mount hooks on `reportMount` |
| `UIManager::getNewestCloneOfShadowNode`, `getRelativeLayoutMetrics` | rerouted to Loom's layout |
| `ShadowTree::getCurrentRevision`, `ShadowTreeRegistry::enumerate/visit` | the surface's registry and its root revision (empty root) |
| `MountingCoordinator::setMountingOverrideDelegate` | accepted; never called, because Loom produces no mounting transactions |
| State updates from native code | rerouted to the node's component |
| Animation backend | rerouted to Loom's animation lane |
| Differentiator, `ShadowViewMutation` lists, `StubViewTree`, Fabric layout animations, the view-transition module, Fabric's mounting manager | no-ops (the code is not compiled; the symbols stay, so libraries still link) |

Every symbol React Native exports stays exported, so a library built against React Native links
and loads. The fallback keeps it working; the Loom API is how you get Loom's behavior and speed.

## 7. Testing

- Run the example screen: `apps/loom-test-app` with `--es scenario example` on Android
  (`-scenario example` on iOS). It prints `[example] {...}` with the renderer, a frame
  measured from native, the result of `setOpacity` and the frame count.
- Build the library both ways: with `loom.renderer=true` (the Loom branch) and with
  `loom.renderer=false` (stock React Native, the Fabric branch).
- A Loom build with `loom.renderer=false` defines `LOOM_RENDERER` as `0`; a stock build has no
  header. Test both.

## 8. Versioning

`LOOM_API_VERSION` starts at 1 and grows only by additions. Compile against what you need and
check it with a `static_assert`; a removal would need a major process, and none is planned.
