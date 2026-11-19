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
static_assert(LOOM_API_VERSION >= 2, "needs Loom API 2");
#else
// Fabric
#endif
```

- `LOOM_RENDERER` is `1` only in Loom builds. A Loom build with `loom.renderer=false` defines it as
  `0`; stock React Native defines nothing and has no `LoomConfig.h`, so the Fabric branch is taken.
- `LOOM_API_VERSION` is the version of Loom's own API (not of React Native). A library compiles
  against the version it needs and checks `LOOM_API_VERSION >= n`. See [Versioning](#_8-versioning).
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
      // called from JS: a synced read includes the commits this thread already made
      if (auto metrics = runtime_.synced().layoutRelativeTo(*surfaceId, tag, std::nullopt)) {
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
  anywhere you have the container. Both return `std::nullopt` when the app does not run Loom. A
  `Runtime` belongs to one React instance: tags are unique only within an instance, and every call
  is answered by that instance alone (two React hosts in one app never see each other's nodes).
- **Surfaces:** `forEachSurface`, `surfaceOf(tag)` (the surface the node is mounted on; `synced().surfaceOf(tag)` also finds a node JavaScript just created).
- **Reads, two explicit modes:**
  - `runtime.latest()` never blocks, on any thread. It answers from the latest computed commit and
    returns `std::nullopt` while a commit is being computed at that instant (try again next frame).
    Use it on the UI thread.
  - `runtime.synced()` first waits until every commit submitted before the call has been applied,
    so JavaScript reads its own writes. On the UI thread it returns `std::nullopt` (use
    `latest()`), except with `loom.js=main`, where the UI thread is the JS thread and applies the
    pending commits itself before reading.
  - Both offer `surfaceSize`, `hasNode`, `props`, `layout`, `layoutRelativeTo`, `componentName`,
    `children` and `parent`, each returning `std::optional`. Inside an `onLayoutCommitted` listener
    both read the commit being reported, at once.
- **Prop writes, three lanes:** `updateProps` (the JS lane, like a React commit),
  `updateAnimatedProps` (the animation lane, a typed `AnimatedPropBag`, coalesced per frame; a layout
  prop makes Loom lay the node out), `updateNativeProps(surfaceId, tag, props)` (paint props are applied to the view and
  remembered so the next commit does not undo it; layout props apply once, like the animation lane, so a later React
  commit that sends the same key replaces them). It returns `false` when the surface has no such node, and it accepts a
  node a submitted commit created that is not mounted yet. Writes of one UI frame are laid out together, in one pass and
  one batch.
- **Native-measured size:** when a view's size comes from native code (a native tree laid out on the UI thread),
  `setMeasuredSize(surfaceId, tag, {width, height, minWidth})` keeps it on the node, apart from the style and React's props:
  a commit that sends the same key does not replace it, and an axis left unset follows the style. An empty size clears
  it. `setInitialMeasuredSize(componentName, provider)` supplies a starting size from the props when a node of that
  component is created, so the first layout is already right (what `adopt()` does under Fabric).
- **Frames:** `onFrameApplied` (UI thread), `onLayoutCommitted` (commit thread),
  `beforeNextFrame(surfaceId, callback)` (runs on the UI thread right before that surface's next
  batch). Subscriptions are RAII: destroying the handle unsubscribes.
- **Native state:** `updateState` for components with their own descriptor and state.
- **Settings:** `setting(LoomSettingId)`.

## 5. Threading rules

- **JS thread:** `Runtime::from(jsi::Runtime&)`.
- **Any thread:** `latest()` reads, `updateProps`, `updateAnimatedProps`, `updateNativeProps`,
  subscriptions.
- **The JS thread, or any thread but the UI thread:** `synced()` reads (they wait for the worker;
  the UI thread never waits for it, except with `loom.js=main`, where it is the JS thread).
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

`LOOM_API_VERSION` versions Loom's own API, never React Native's. Each Loom release supports exactly
one React Native minor (Loom 0.87.x runs only on React Native 0.87.x; packaging enforces it), so the
API version does not move with React Native.

- Additions keep the version. The `Runtime` handle holds only a pointer to its instance, so adding
  methods never changes its size or layout.
- An incompatible change bumps it, and is listed in `react/renderer/LoomConfig.h`.
  - **2:** `Runtime` is per React instance; reads became explicit (`latest()`, `synced()`);
    `updateNativeProps` takes the `SurfaceId`; `surfaceOf(tag)` was added.
- Check the version you compile against with a `static_assert`. `Runtime::from` also checks it at
  run time: a library compiled against an API this Loom no longer provides gets `std::nullopt` (and
  one log line), and takes its Fabric path instead of misbehaving.
