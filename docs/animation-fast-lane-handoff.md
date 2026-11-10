# Handoff — animation fast lane: what we built, and the launch crash that blocks it

## Follow-up: launch crash diagnosis

The instrumented APK confirmed `clsfy init paintObj=0 layoutObj=0`, followed by a
valid `transform` array payload and the null-object exception on the paint write.
The lane's `facebook::react::AnimatedProps` collided with upstream's unrelated
type in `ReactCommon/react/renderer/animationbackend/AnimatedProps.h`. The two
definitions violate the C++ one-definition rule; the backend's constructor can
be selected at link time, leaving the lane's dynamic members null.

The lane type is now `ClassifiedAnimatedProps`. Both headers are included together
in the classifier regression tests to prevent this collision from returning.
Temporary `RNXLaneDbg` instrumentation has been removed. The overlay initialization
fix remains, and `zIndex` now follows the layout path for child reordering.

Verification: the all-on arm64 debug APK builds and launches on `QV7120NC26`.
All 87 C++ tests pass on that device, including both classifier regressions.
The materializer also now preserves generated Android build directories and
Gradle/CMake caches across override refreshes and `--force`; its Node regression
test passes. See [verification.md](verification.md#animation-fast-lane-launch-crash-fix)
for the commands and device evidence.

The same fix also builds and runs on the iPhone 17 Pro / iOS 26.5 Simulator.
LLDB confirmed that Native Animated enters the renamed classifier on the main
thread; the animation, smoke controls, and repeated launches remain functional.
No additional native source changes were needed. The original handoff below is
retained as historical debugging evidence, including its unverified iOS claims.

---

Status: **the feature is implemented (Android, design steps 0–5 + 8) but the app crashes on
launch, ~120 ms after mount, and the React instance is torn down.** Everything below is the
current state as of this note, including the exact evidence collected so far, the dead ends,
and the ranked next steps. No fast-lane code has been committed yet.

---

## 0. TL;DR — what to do first

1. The crash is a **lane regression** (proven by running the pre-lane APK against the same JS
   bundle). Breadcrumbs on the device have narrowed the throw to a **single statement**:
   `auto classified = classifyAnimatedProps(props);` in `recordNativeProps`
   (`packages/react-native-resha/src/ReactCommon/react/renderer/uimanager/UIThreadRendererBinding.cpp`).
2. Finer breadcrumbs (12 log lines) are **already written into the source** of
   `UIThreadRenderer.cpp` but have **never been compiled**. First action: materialize + build +
   install + read `adb logcat -s RNXLaneDbg` (commands in §6). The last breadcrumb that prints
   names the failing operation, and the per-item line also reveals the real payload shape.
3. Static analysis (see §5.5) says every operation in that call tree is guarded, so the
   hypothesis to test first is: **`AnimatedProps classified;` is not getting its member
   initializers in the compiled TU** (the `clsfy init paintObj=%d layoutObj=%d` breadcrumb
   answers this immediately). If `layoutObj=0`/`paintObj=0`, look for an ODR/ABI mismatch or a
   stale `UIThreadRenderer.h` in the build's include path — not for a bad payload.
4. Do **not** re-litigate the already-fixed overlay bug (§5.6); it was real but was not the
   crash.

---

## 1. Where things are

| Thing | Path |
| --- | --- |
| Working repo (all changes happen here) | `/Users/ammarahmed/Work/react-native-resha` |
| Read-only reference fork (**do not modify**) | `/Users/ammarahmed/Work/resha` |
| Design doc, the 1:1 spec (426 lines) | `/Users/ammarahmed/Work/resha/docs/resha/animation-fast-lane-design.md` |
| Package under test | `packages/react-native-resha` |
| Test app | `apps/resha-test-app` |
| Materialized RN tree (what actually compiles) | `apps/resha-test-app/node_modules/.resha/react-native` |
| Device | `QV7120NC26` (Sony SO-01M, arm64-v8a, API 30, 420 dpi) |

Hard constraints from the user: **never push** (`m00062`), **never modify
`/Users/ammarahmed/Work/resha`**, **do not publish the package** (`m01288`), it must be built
as **overrides on pristine npm react-native 0.87.1** (no fork).

### How the override system works (needed to edit anything)

- Manifest: `packages/react-native-resha/overrides.json` — `{"upstream":{...},"overrides":[…]}`.
  Both `patch` and `platform` entries store the **full modified file** under
  `packages/react-native-resha/src/<react-native-relative-path>`.
- `npx resha materialize [--force]` (run from the app) copies those files into
  `node_modules/.resha/react-native/…`; `--force` also removes files whose override was deleted.
- `node packages/react-native-resha/bin/resha.js validate` (from the repo root) checks
  `baseHash` against pristine upstream. Currently: **92 overrides valid**.
- Add a new override: `node packages/react-native-resha/bin/resha.js add <rn-relative-path>
  --area <area>`; non-upstream files use `add --platform <path>`.
- **Always grep the materialized file after materializing** — the edit pipeline silently drops
  writes now and then (see §9).

### Build switches (all-on is the intended default config)

`-PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true`
(plus `-PRN_PROFILE_UI_THREAD_RENDERER=true` to get the lane profile line).
Prerequisite: `env -u ANDROID_NDK` — the env var breaks the NDK resolution (`CXX1100`).
Note: Fabric **cannot** be turned off on RN 0.87.1 (`ReactRootProjectPlugin.kt:23` forces
`newArchEnabled`), so the only axis tested is all-off vs all-on of the Resha trio.

---

## 2. What the fast lane is (design doc §0–§9, condensed)

Animations on the UI-thread renderer previously went through `setNativeProps`-style
`stageNativeProps`, i.e. a full ordinary commit per write. The fast lane replaces that for
animation traffic:

- **Coalesce, don't queue.** Writes land in a per-surface frame buffer keyed by tag, last write
  wins; one flush per frame (end of JS task via the RuntimeScheduler; Android pre-draw as a
  safety net).
- **Classify per key**: *paint* keys are applied to the native view immediately and folded with
  no Yoga/diff; *layout* keys need Yoga and go through patchProps + one commit + one `applyBatch`;
  *text* keys always take the ordinary path.
- **Never block on the worker.** Layout frames run synchronously on the UI thread when the worker
  is idle (`runIfIdle`); otherwise they merge into **one** pending coalesced job per surface
  (replace, not append → ≤1 frame of lag, and the design explicitly forbids accumulating).
- **Adaptive budget**: per-surface EMA of the synchronous lane cost; over budget for 3 consecutive
  frames → async mode; while async, one sampled synchronous probe every 30 frames; back to sync
  once the probe EMA drops below budget/2. Default budget 2 ms @120 Hz, 4 ms @60 Hz.
- **Correctness vs in-flight React commits**: React wins for the same key; an animated value
  otherwise persists because it lives in the node's merged props; the overlay is kept only for
  paint keys of tags whose frame has not yet been folded.

Cost model in the doc: a steady-state full worker commit is ≈143 µs for ~400 nodes.

---

## 3. What is implemented

### 3.1 Classifier (step 2) — `ReactCommon/react/renderer/uimanager/UIThreadRenderer.{h,cpp}`

- `UIThreadRenderer.h:126` `enum class AnimatedPropClass { Paint, Layout, Text };`
- `UIThreadRenderer.h:129-141`
  ```cpp
  struct AnimatedProps {
    folly::dynamic paint{folly::dynamic::object()};
    folly::dynamic layout{folly::dynamic::object()};
    bool text{false};
    bool empty() const { return paint.empty() && layout.empty() && !text; }
  };
  ```
- `UIThreadRenderer.h:147` `AnimatedProps classifyAnimatedProps(const folly::dynamic& props);`
- `UIThreadRenderer.cpp:372-400` `classifyAnimatedPropKey(const std::string& key)`:
  - **Paint**: `opacity`, `transform`, `transformOrigin`, `backgroundColor`, `tintColor`,
    `overlayColor`, `elevation`, `border*Color`, `border*Radius`, `shadow*`.
  - **Text**: `font*`, `lineHeight`, `letterSpacing`, `textAlign`, `textAlignVertical`,
    `textTransform`, `textDecorationLine`, `textShadow*`, `allowFontScaling`, `numberOfLines`.
  - Everything else (including `zIndex`) → **Layout**.
- `UIThreadRenderer.cpp:402-431` `classifyAnimatedProps` hoists via `hoistProps` and returns early
  unless the result is an object.

### 3.2 Frame buffer + flush (steps 1, 3, 4) — `UIThreadRendererBinding.{h,cpp}`

- `UIThreadRendererBinding.h` (private fast-lane block, ≈:357-420):
  `struct AnimatedWrite { uint64_t identity{0}; folly::dynamic paint{…object()}; folly::dynamic
  layout{…object()}; bool text{false}; }`, `std::unordered_map<SurfaceId,
  std::unordered_map<Tag, AnimatedWrite>> animationFrameBuffer_`,
  `struct AnimationFrame { std::mutex mutex; bool claimed{false}; std::unordered_map<Tag,
  folly::dynamic> paint, layout; std::vector<Tag> tags; bool merge(...); bool take(...); }`,
  `pendingAnimationFrames_`, `struct AnimationLaneStats { double emaMicros{0}; int
  overBudgetFrames{0}; int asyncProbeCounter{0}; bool asyncMode{false}; }`, `animationLaneStats_`.
- `UIThreadRendererBinding.cpp` — the lane block is `#if UI_THREAD_RENDERER_WORKER` at
  ≈:425-876, with constants `kAnimationLaneBudgetMicros = 2000.0`,
  `kAnimationLaneOverBudgetFrames = 3`, `kAnimationLaneProbeFrames = 30`:
  - `writeAnimatedProps(surfaceId, tag, identity, classified, applyPaintToView)` (≈:534-575):
    merges into the frame buffer, resets the write when the identity changes, ORs the text flag,
    and — only when the paint object is non-empty — writes **only the paint keys** into
    `nativePropsOverlays_[surfaceId][tag]` with `overlay.sequence = 0` (marks it lane-owned so no
    staged operation erases it), then optionally calls `applyAnimatedPaintToView`.
  - `applyAnimatedPaintToView` (≈:577-600): saves/restores `replayingNativePropsTag_`/
    `replayingNativeProps_` around `delegate->uiManagerShouldSynchronouslyUpdateViewOnUIThread`.
  - `erasePaintOverlays` (≈:602-617): erases only entries with `sequence == 0`.
  - `recordAnimationLaneCost` / `animationLaneAllowsSync` / `traceAnimationLane` (≈:619-691):
    EMA seeds on the first sample then `ema*0.8 + micros*0.2`; async exits when the probe EMA
    drops below budget/2; `++asyncProbeCounter % 30 == 0` allows a synchronous probe.
  - `flushAnimationFrames()` (≈:694, **public static** — see the header) iterates surfaces.
  - `flushAnimationFrame(SurfaceId)` (≈:712): moves the per-surface write map out and erases the
    entry **before** applying anything (folding re-enters the map through the synchronous
    delegate), drops writes whose tag is missing or whose identity changed, then calls
    `deferAnimationFrame`.
  - `deferAnimationFrame` (≈:746): merges into an unclaimed `pendingAnimationFrames_[surfaceId]`
    (one job per surface) or builds the job —
    `job->compute = [rendererOwner, frame] { … if (!frame->take(paint, layout)) return;
    rendererOwner->applyAnimationFrame(paint, layout); }`, `job->applied = [this, surfaceId,
    frame] { copy frame->tags; erasePaintOverlays(surfaceId, tags); erase
    pendingAnimationFrames_[surfaceId] when it still points at frame; }`; the token comes from
    `surfaceTokens_.find(surfaceId)` (**never** `tokenFor`, which would create tokens);
    `pendingAnimationFrames_[surfaceId] = frame` is set before submitting. The synchronous branch
    measures `UIThreadRendererWorkerProfile::Clock::now()` around `submitWorkerJob(token,
    std::move(job), &ranSynchronously)` and records cost only when it really ran synchronously.
  - `traceAnimationLane` is `#if RN_PROFILE_UI_THREAD_RENDERER` +
    `UIThreadRendererWorkerProfile::enabled()` gated and logs
    `RNXAnimLane sync=%d cost=%.0fus nodes=%zu surface=%d` (Android `__android_log_print`,
    Apple `os_log`).
- Entry points:
  - `recordNativeProps` (≈:376-423): echo guard kept; `mountedTokens_`/`renderers_`/
    `mountedIdentities_` are all **`find`-based and non-throwing** (this is the step-0 C1 fix;
    the old `mountedIdentities_.at(tag)` is gone) → `classifyAnimatedProps(props)` → text payload
    or text node → `stageNativeProps`; otherwise → `writeAnimatedProps(…,
    /*applyPaintToView=*/false)`.
  - `setWorkerNativeProps` (≈:821) has an `if (isMounted)` fast-lane branch (≈:838-850) that
    routes non-text writes to `writeAnimatedProps(…, /*applyPaintToView=*/true)`.
  - `releaseSurfaceInternal` also erases `animationFrameBuffer_`, `pendingAnimationFrames_` and
    `animationLaneStats_`.
- The worker's `submitWorkerJob(token, job, bool* ranSynchronously)` sets `*ranSynchronously =
  false` on entry and `true` before returning from its `runIfIdle` branch.
- Identity semantics: `identityOf(tag)` returns **0 when the node is missing**, so 0 doubles as
  an "unknown" sentinel; `mountedIdentities_[tag] = identity` is written in
  `applyPublishedIdentities` (≈:1558); `AnimatedWrite` resets when the identity changes.

### 3.3 Renderer side (step 3) — `UIThreadRenderer.{h,cpp}`

- `UIThreadRenderer.h:350` `void foldPaintProps(Tag tag, const folly::dynamic& paint);` — folds
  paint keys into the node's merged props with no style rebuild / no commit.
- `UIThreadRenderer.h:360` `void applyAnimationFrame(const std::vector<std::pair<Tag,
  folly::dynamic>>& paintPatches, const std::vector<std::pair<Tag, folly::dynamic>>&
  layoutPatches);` — paint via `foldPaintProps`, layout via `patchProps` + **one** `commit()` +
  one `applyBatch`.
- Existing machinery reused: `tryCommitPaintOnlyProps` (`UIThreadRenderer.cpp:2511-2572`),
  `mergeProps` (`:464-497`), `refreshStyle` (`:505-554`), `emitUpdateProps` (`:1054-1060`),
  `beginCommitLayout`/`finishCommitAfterLayout`/`flush` (`:1482`/`:1577`/`:1956`).

### 3.4 Flush hooks (step 5, Android)

- **RuntimeScheduler rendering-update listener.** New overrides (area `threading`):
  `RuntimeScheduler.h` (base virtual `setRenderingUpdateListener(std::function<void()>) {}` — not
  pure, so the legacy fork keeps compiling; plus the proxy `override`), `RuntimeScheduler.cpp`
  (proxy forwards), `RuntimeScheduler_Modern.h` (declaration ≈:149 + member
  `std::function<void()> renderingUpdateListener_;` ≈:231), `RuntimeScheduler_Modern.cpp`
  (setter ≈:323, and at the **end of `updateRendering`**, after the FIFO drain, ≈:505-507:
  `if (renderingUpdateListener_ != nullptr) { try { renderingUpdateListener_(); } catch (...) {} }`).
  The listener is installed in `UIManagerBinding.cpp` inside `createAndInstallIfNeeded`
  (≈:49-60): `RuntimeSchedulerBinding::getBinding(runtime)` → `getRuntimeScheduler()` →
  `setRenderingUpdateListener(&UIThreadRendererBinding::flushAnimationFrames)`.
  **Why there and not in `RuntimeSchedulerBinding.cpp`:** the `react_renderer_runtimescheduler`
  CMake target's include path does not contain `react/renderer/graphics/platform/android`, so
  including `UIThreadRendererBinding.h` (which pulls `Color.h`) from that TU fails with
  `fatal error: 'react/renderer/graphics/HostPlatformColor.h' file not found`. The
  `runtimescheduler/` TU cannot see renderer headers; the `uimanager/` TU can. A first attempt
  did live in `RuntimeSchedulerBinding.cpp` and was deleted (override entry removed too).
- **Android pre-draw safety net.** `FabricUIManagerBinding.kt` gained
  `external fun flushDirectAnimationFrame()`; `FabricUIManagerBinding.h` gained
  `void flushDirectAnimationFrame();`; `FabricUIManagerBinding.cpp` implements it
  (`#if UI_THREAD_RENDERER_WORKER` → `UIThreadRendererBinding::flushAnimationFrames();`) and
  registers `makeNativeMethod("flushDirectAnimationFrame", …)` as the second `registerHybrid`
  entry; `FabricUIManager.java` `DispatchUIFrameCallback.doFrameGuarded` calls it (guarded by
  `BuildConfig.UI_THREAD_RENDERER_WORKER && mBinding != null`, wrapped in try/catch) **after**
  `drainPreallocateViewsQueue()` and **before** the pre-mount block — so the existing
  `finally { schedule(); }` always re-arms the frame callback and a flush failure cannot disable
  mounting. `ReactChoreographer` runs `NATIVE_ANIMATED_MODULE` before `DISPATCH_UI`, so this one
  net covers both the Native Animated end-of-frame and JS tasks that bypass `runEventLoopTick`.
- `flushAnimationFrames()` is declared **unconditionally** in the public part of
  `UIThreadRendererBinding.h` (≈:100) so the function pointer can be taken; every *reference* to
  it is guarded by `#if UI_THREAD_RENDERER_WORKER`, because the non-worker build must keep
  compiling unchanged.

### 3.5 Step 6 and 7 — not implemented (deliberately)

- **Step 6 (C++ Native Animated direct adapter) is untouched.** `cxxNativeAnimatedEnabled()`
  defaults to **false** (`ReactCommon/react/featureflags/ReactNativeFeatureFlagsDefaults.h:38`),
  so the C++ `NativeAnimatedNodesManagerProvider` path is dormant; on Android the Kotlin
  `NativeAnimatedModule` → `PropsAnimatedNode.updateView` →
  `FabricUIManager.synchronouslyUpdateViewOnUIThread` → `recordDirectNativeProps` already routes
  into the lane. Relevant for later: `NativeAnimatedNodesManagerProvider.cpp:63-75` — PATH 1's
  `fabricCommitCallback` wraps `uiManager->updateShadowTree(tagToProps)`, and
  `AnimatedMountingOverrideDelegate` is registered for existing + future surfaces
  (≈:107-129); a direct-mode adapter would replace exactly those.
- **Step 7 (iOS paint through the direct applier's descriptor registry) is impossible in this
  repo** — the package contains **no Apple/iOS files at all** (all 22 platform overrides are
  Android or C++). The same is true for the iOS half of step 5 (CFRunLoopObserver /
  `RCTSurfacePresenter.mm`).

### 3.6 Three conformance edits made after the first build

1. `zIndex` moved from the Paint list to **Layout**
   (`UIThreadRenderer.cpp:372-400`) — RN implements it by reordering children in the mounting
   layer, so it cannot be treated as a pure view-level paint.
   ⚠️ The comment block just above still justifies `zIndex` as paint — **stale comment, fix it.**
2. The adaptive budget is gated only on layout/text frames
   (`deferAnimationFrame`): `const bool hasLayoutOrText = !layoutPatches.empty() || needsAsync;
   const bool synchronous = !hasLayoutOrText || animationLaneAllowsSync(surfaceId);`
   → paint-only frames are always immediate.
3. Both entry points route **text nodes** to the ordinary path even when the payload is not
   classified as text: `recordNativeProps` uses `classified.text ||
   owner->second->isTextNode(tag)`; `setWorkerNativeProps` uses `!classified.text &&
   (rendererIt == renderers_.end() || !rendererIt->second->isTextNode(ref.tag))` with
   `const auto rendererIt = renderers_.find(ref.surfaceId);` declared inside the `if (isMounted)`
   block.

---

## 4. What is broken

### 4.1 Symptom

Launching the all-on build and mounting the smoke screen crashes the React instance ~120 ms
after mount, before any animation frame is applied. Exact logcat (build 7, pid 18036):

```
14:10:43.401 I/ReactNativeJS: [resha-smoke] mounted
14:10:43.520 I/RNXLaneDbg: rec tag=62 obj=1 size=1
14:10:43.521 W/unknown:BridgelessReact: ReactHost{0}.handleHostException(
                 message = "java.lang.RuntimeException: TypeError: expected dynamic type 'object', but had type 'null'")
```

The `Caused by` chain:

```
java.lang.RuntimeException: TypeError: expected dynamic type 'object', but had type 'null'
  at com.facebook.react.fabric.FabricUIManagerBinding.recordDirectNativeProps(Native Method)
  at com.facebook.react.fabric.FabricUIManager.synchronouslyUpdateViewOnUIThread(FabricUIManager.java:938)
  at com.facebook.react.animated.PropsAnimatedNode.updateView(PropsAnimatedNode.kt:105)
  at com.facebook.react.animated.NativeAnimatedNodesManager.updateNodes(NativeAnimatedNodesManager.kt:707)
  at com.facebook.react.animated.NativeAnimatedNodesManager.runUpdates(NativeAnimatedNodesManager.kt:584)
  at com.facebook.react.animated.NativeAnimatedModule$animatedFrameCallback$1.doFrameGuarded(NativeAnimatedModule.kt:350)
```

Then `[resha-smoke] unmounted`, `Tried to remove non-existent frame callback` ×3,
`Scheduler::~Scheduler() was called`, `UIManagerBinding::~UIManagerBinding()`,
`UIManager::~UIManager()` — the instance is destroyed, so **no further on-device verification of
the lane is possible until this is fixed.**

The Java hop that reaches the lane
(`node_modules/.resha/react-native/ReactAndroid/src/main/java/com/facebook/react/fabric/FabricUIManager.java:930-940`):

```java
public void synchronouslyUpdateViewOnUIThread(final int reactTag, final ReadableMap props) {
  if (BuildConfig.UI_THREAD_RENDERER_WORKER && mBinding != null) {
    WritableNativeMap delta = Arguments.makeNativeMap(props.toHashMap());
    mBinding.recordDirectNativeProps(reactTag, delta);   // ← line 938 in the stack
  } else { … }
}
```

JNI hop (`ReactAndroid/src/main/jni/react/fabric/FabricUIManagerBinding.cpp:1351-1358`):

```cpp
void FabricUIManagerBinding::recordDirectNativeProps(jint tag, NativeMap* props) {
#if UI_THREAD_RENDERER_WORKER
  UIThreadRendererBinding::recordNativeProps(tag, props->consume());
#else
  (void)tag; (void)props;
#endif
}
```

### 4.2 It is a lane regression — proven

Installing the **pre-lane** all-on APK `/tmp/resha-allon-2ec09dfa.apk` (65,155,033 B, sha256
`588e51691e7086db3edf784a52cb89ed65605c1dc11d547b30b9e4aade0f7235`) against the **same Metro JS
bundle** produces no crash at all — logcat only shows `[resha-smoke] mounted` and (expected)
`build switches unavailable: ReshaBuildConfig native module not registered`. So the failure was
introduced by the fast-lane changes, not by the smoke test or the JNI plumbing.

### 4.3 Where it throws (breadcrumb evidence)

Build 7 carried test-only breadcrumbs in the materialized `UIThreadRendererBinding.cpp`, in
`recordNativeProps`:

```
rec     → immediately before `auto classified = classifyAnimatedProps(props);`
cls     → immediately after it (prints the classified sizes)
toStage → inside the text branch, before stageNativeProps
toLane  → before writeAnimatedProps
```

`rec` printed (`obj=1 size=1` → the payload is a valid object with exactly one key), `cls` never
printed. The only statement between them is the `classifyAnimatedProps(props)` call, so **the
throw happens inside `classifyAnimatedProps` or inside `hoistProps`/`mergeStyleInto`, which it
calls** (`UIThreadRenderer.cpp:172-250` and `:402-431`).

The smoke test animates one thing: `Animated.loop(Animated.sequence([timing(anim, {toValue:1,
duration:800, useNativeDriver:true}), timing(anim,{toValue:0, …})]))` on
`<Animated.View style={[styles.animated, {transform:[{translateX: anim.interpolate(...)}]}]} />`
(`apps/resha-test-app/ReshaSmokeTest.tsx:113-131`, target :219-237) — i.e. the payload should be
a single **paint** key (`transform`), which the classifier handles with
`classified.paint[key] = item.second;` on an object it just created. That is why the payload
being "weird" is a weak hypothesis and the struct-initialization/ODR hypothesis is stronger.

### 4.4 The folly mechanism behind the exact message

Read from the copy this build links against
(`node_modules/.resha/react-native/ReactAndroid/build/third-party-ndk/folly/folly/json/dynamic.{h,cpp}`):

- `dynamic.cpp:207-213` `dynamic const& dynamic::at(StringPiece idx) const&`:
  ```cpp
  auto* pobject = get_nothrow<ObjectImpl>();
  if (!pobject) { throw_exception<TypeError>("object", type()); }   // ← our exact message
  ```
- `dynamic.cpp:279-286` `const dynamic* dynamic::get_ptr(StringPiece idx) const&` — the same
  explicit throw; **`find()` routes here**, and its doc comment says "Throws if this is not an
  object."
- `dynamic.cpp:225-229` `dynamic& dynamic::operator[](StringPiece k) &`:
  ```cpp
  auto& obj = get<ObjectImpl>();          // ← unguarded; same message when *this is null
  auto ret = obj.emplace(k, nullptr);
  ```
- `items()`/`keys()`/`values()` are built via `get<ObjectImpl>()` too.
- `dynamic::size()` (`dynamic.cpp:297`) uses `get_nothrow` for Array/ObjectImpl/`std::string`
  and only then throws `TypeError("array/object/string", type())` — a **different** message, so
  `size()` is *not* the throw.
- The **dynamic-key** overloads (`atImpl(dynamic const&)`, `at(dynamic const& idx)`) throw
  `TypeError("object/array", type())` — also a different message.

⇒ The null dynamic in the crash is being accessed with a **string key** (or via
`items()`/`keys()`/`values()`), and the receiver's tag is genuinely `NULLT`.

### 4.5 Every string-key access on that path, and why static analysis says "impossible"

Walked call tree of `classifyAnimatedProps(props)` with the receiver each candidate would throw on:

| Site | Receiver | Guarded? |
| --- | --- | --- |
| `classifyAnimatedProps`: `classified.paint[key] = item.second;` | `classified.paint` (member init `…object()`) | no guard — but should be an object |
| `classifyAnimatedProps`: `classified.layout[key] = item.second;` | `classified.layout` (member init) | same |
| `classifyAnimatedProps`: `classified.layout[item.first] = item.second;` (non-string key) | — | would say `'object/array'`, not `'null'` |
| `hoistProps`: `props.count("style")` | payload `props` | `!props.isObject() \|\|` short-circuit ✓ |
| `hoistProps`: `flattened["style"] = nullptr;` | `flattened` (fresh object) | ✓ |
| `hoistProps`: `flattened[item.first] = item.second;` | dynamic key → `atImpl` | would say `'object/array'` |
| `mergeStyleInto`: `style.items()` (object branch) | `style` | `if (style.isObject())` ✓ |
| `mergeStyleInto`: `props[entry.first] = entry.second;` | dynamic key | would say `'object/array'` |
| `mergeStyleInto` null branch: `props.find(entry.name) != props.items().end()` | accumulator (`flattened`) | reached only for `style: null`; accumulator is an object |
| `mergeStyleInto` null branch: `props[entry.name] = nullptr;` | accumulator | same |

The `rec` breadcrumb also proves the payload is an object with one key at the moment of the
call. So **no operation in the tree can produce this message** — unless one of these is true:

1. **`AnimatedProps`' member initializers are not in effect in the compiled
   `UIThreadRenderer.cpp` TU** (making `classified.paint` / `classified.layout` null). This is
   the leading hypothesis: it is the only unguarded string-key receiver, and it explains why the
   throw appears on the very first frame with an innocent payload.
   Worth checking: a second/older `UIThreadRenderer.h` on the include path, an ODR conflict
   (another `AnimatedProps` in the same namespace), or the build using a different
   `react/renderer/uimanager` copy than the one that was inspected.
   *(A grep at the time found only one copy of each renderer header, all under
   `apps/resha-test-app/node_modules/.resha/react-native/ReactCommon/react/renderer/uimanager/`,
   but the compiled include path itself was never verified directly — do that first.)*
2. The payload's real shape is different from what the smoke test appears to send (the
   per-item breadcrumb in §5.7 reveals the actual key and value types).
3. Heap corruption/dangling `folly::dynamic` — least likely, since the `rec` line shows sane
   type and size, but not impossible if something else in the lane writes out of bounds.

### 4.6 A fix that was applied and did **not** fix the crash

`struct NativePropsOverlay` (`UIThreadRendererBinding.h:346-351`) had **no default member
initializers**, so a fresh `nativePropsOverlays_[surfaceId][tag]` was value-initialized with
`identity = 0` and `props` = **null**; both writers skipped their reset when the identity
matched, and `overlay.props[key] = value` then indexed a null dynamic — producing exactly this
`TypeError`. It is now:

```cpp
struct NativePropsOverlay {
  SurfaceId surfaceId{0};
  uint64_t identity{0};
  uint64_t sequence{0};
  folly::dynamic props{folly::dynamic::object()};
};
```

plus a belt-and-braces reset in `writeAnimatedProps` (`≈:566`):
`if (overlay.identity != identity || !overlay.props.isObject()) { overlay.props =
folly::dynamic::object(); }`.

This was a real latent bug and should be kept — but build 6 (which contained it) still crashed
identically, so it is **not** the current blocker. Do not spend more time on the overlay.

### 4.7 Instrumentation currently in the tree (**not yet compiled**)

- `UIThreadRendererBinding.cpp` — 7 `RNXLaneDbg` logs (`rec`, `cls`, `toStage`, `toLane`,
  `wap`, `ovl`, `stage`). **These are in the build 7 APK.**
- `UIThreadRenderer.cpp` — 12 `RNXLaneDbg` logs, verified present in the source at 6 anchors:
  `classifyAnimatedProps` entry / `clsfy init paintObj=%d layoutObj=%d` (right after
  `AnimatedProps classified;`) / `clsfy hoisted obj=%d isNull=%d` / a per-item line printing the
  key and `vNull/vObj/vArr/vStr` / `clsfy -> paint paintObj=%d` / `clsfy -> layout (nonstring)
  paintObj=%d`; `hoistProps` `hoist enter obj=%d hasStyle=%d`; `mergeStyleInto`
  `merge enter accObj=… sObj=… sArr=… sNull=…`, `merge obj-branch`, `merge null-branch`,
  `merge null-branch hit`.
  **These have never been materialized or built** (two build attempts were dropped; see §9).
  They are test-only scaffolding: remove them once the bug is understood, and note that
  `mergeStyleInto`/`hoistProps` run for *every* props payload, so the log volume is high —
  filter with `adb logcat -s RNXLaneDbg` and expect a flood.
- Backups of the pre-instrumentation files: `/tmp/UIThreadRenderer.cpp.bak`,
  `/tmp/UIThreadRendererBinding.cpp.bak`.

---

## 5. Reproduction procedure (exact commands)

```bash
# 1. materialize the overrides into the test app (from the app dir)
cd /Users/ammarahmed/Work/react-native-resha/apps/resha-test-app
npx resha materialize --force          # expect: "react-native@0.87.1 + 87 overrides"

# 2. verify the edit actually landed in the tree that compiles (writes drop silently!)
RN=node_modules/.resha/react-native
grep -c RNXLaneDbg $RN/ReactCommon/react/renderer/uimanager/UIThreadRenderer.cpp   # expect 12

# 3. build the all-on debug APK (arm64 only for speed)
cd android
env -u ANDROID_NDK ./gradlew :app:assembleDebug \
  -PreactNativeArchitectures=arm64-v8a \
  -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true
# APK: app/build/outputs/apk/debug/app-debug.apk   (~65 MB, ~2-4 min with ccache warm)

# 4. device
adb -s QV7120NC26 install -r -t app/build/outputs/apk/debug/app-debug.apk
adb -s QV7120NC26 reverse tcp:8081 tcp:8081        # Metro must be serving 8081
adb -s QV7120NC26 shell am start -n com.reshatestapp/.MainActivity
adb -s QV7120NC26 logcat -c
adb -s QV7120NC26 logcat -s RNXLaneDbg:V ReactNativeJS:V BridgelessReact:W unknown:W
```

The switch banner + `[resha-smoke]` lines on screen report the compiled configuration; the JS
thread id equals the process id when the UI thread renderer is on.

Useful context: the smoke screen (`apps/resha-test-app/ReshaSmokeTest.tsx`) has a 60 Hz JS
ticker, the native-driver looping animation (the crash trigger), a rAF burst pump, a remount
button and the three-line switch banner. Device input note: `adb shell input tap X Y` uses
physical pixels 1:1 (pageX/pageY in JS are dp; ÷2.625 at 420 dpi).

---

## 6. Ranked next steps

1. **Build + run the instrumented tree** (§5.7 / §7 commands) and read `RNXLaneDbg`. The last
   line before the `TypeError` names the failing operation; the per-item line reveals the actual
   payload key/types.
2. **Answer the leading hypothesis directly**: the `clsfy init paintObj=%d layoutObj=%d` line.
   - If either is `0`, the member initializers did not apply → find out which header/TU the
     compiler used for `UIThreadRenderer.h` (check the actual `-I` flags / compile command in
     `build/intermediates` or `build/.cxx`; grep for a second `AnimatedProps` definition
     anywhere in the include path; consider `enum`/struct layout mismatch with
     `UIThreadRendererInternals.h`).
     A pragmatic stopgap in that case is to stop relying on NSDMIs and initialize explicitly
     (`classified.paint = folly::dynamic::object();` right after construction) — but diagnose
     first, because the same mismatch could affect other lane structs.
   - If both are `1`, the null is elsewhere and the trace will point at it.
3. **Reproduce the classifier in isolation in the C++ unit tests** — this is fast and does not
   need a device: `packages/react-native-resha/src/ReactCommon/react/renderer/uimanager/tests/UIThreadRendererTest.cpp`
   already has a fixture that can call the public `classifyAnimatedProps` /
   `tryCommitPaintOnlyProps` API (`UIThreadRenderer.h:126/:129/:147/:336/:350/:360`). Feed it
   `{"transform":[{"translateX":0}]}`, `{"style":{"transform":[…]}}`, `{"style":null}`,
   `{"opacity":0.5}`, a non-object payload, and an object with a non-string key. If the suite
   crashes or throws there, you have the bug without the device.
   Run the C++ suites per `.github/workflows/react-native-tests.yml` (`resha overlay --monorepo`
   into a v0.87.1 checkout, then
   `:packages:react-native:ReactAndroid:assembleDebug -PreactNativeArchitectures=x86_64
   -PRN_BUILD_CPP_TESTS=true`; the test executable is staged under
   `build/intermediates/cxx/<buildType>/<hash>/obj/<abi>/ui_thread_renderer_tests`).
4. Only after the crash is gone: add the lane-specific tests (coalescing/last-write-wins,
   identity change, paint-only frame applies without a commit, one pending job per surface, text
   routing, stale tag dropped, unmount mid-animation), capture `RNXAnimLane` numbers with
   `-PRN_PROFILE_UI_THREAD_RENDERER=true`, update `docs/verification.md`, and commit.

Separate latent hazard worth a look while you are in there (not the crash):
`isLayoutOnlyView` (`UIThreadRenderer.cpp:269-311`) iterates `props.items()` at `:285`
**without** an object guard, unlike its neighbours.

---

## 7. Repo state

- Last commits: `4ad0573 docs: record the all-off and all-on device matrix`,
  `f1e92e1 test(app): report the compiled switch configuration on the smoke screen`,
  `3899276 docs: record the on-device smoke test and the three runtime defects it found`,
  `382b08f test(app): run a direct renderer smoke test on device`. **Nothing about the fast lane
  is committed yet — 27 commits total, nothing pushed.**
- Uncommitted: 11 modified —
  `packages/react-native-resha/overrides.json`,
  `packages/react-native-resha/src/ReactAndroid/src/main/java/com/facebook/react/fabric/FabricUIManager.java`,
  `.../fabric/FabricUIManagerBinding.kt`,
  `packages/react-native-resha/src/ReactAndroid/src/main/jni/react/fabric/FabricUIManagerBinding.cpp`,
  `.../FabricUIManagerBinding.h`,
  `packages/react-native-resha/src/ReactCommon/react/renderer/runtimescheduler/RuntimeScheduler_Modern.cpp`,
  `.../uimanager/UIManagerBinding.cpp`,
  `.../uimanager/UIThreadRenderer.cpp`, `.../uimanager/UIThreadRenderer.h`,
  `.../uimanager/UIThreadRendererBinding.cpp`, `.../uimanager/UIThreadRendererBinding.h`;
  3 untracked — `RuntimeScheduler.cpp`, `RuntimeScheduler.h`, `RuntimeScheduler_Modern.h` (all
  under `.../react/renderer/runtimescheduler/`).
- Manifest: 92 overrides valid (was 89 before the lane; the `RuntimeSchedulerBinding.cpp`
  override was added and then removed again).
- Build artifacts / logs referenced in this note: `/tmp/animlane-build5.log` (first APK with the
  lane), `/tmp/animlane-build6.log` (overlay fix — still crashed), `/tmp/animlane-build7.log`
  (breadcrumb build — the one installed on the device now, APK 65,294,049 B, sha256 prefix
  `4c8356fbd41e2e7df1e8029fdc0989eb`). Build 8 (classifier breadcrumbs) was never built.

### Tooling hazards (cost hours if ignored)

- **One tool call per assistant message.** When two calls are batched, the later one is
  silently dropped — this includes `run_in_background` bash calls (a whole build was lost this
  way) and `git add`/`commit` batches.
- `write`/`edit` sometimes report success without persisting. **Always verify with `grep`** and
  re-apply if needed. This is how the `RuntimeScheduler_Modern.h` declaration was lost once,
  which cost a full build cycle.
- Local builds: `env -u ANDROID_NDK`; `ccache` at `/opt/homebrew/bin/ccache` makes rebuilds
  minutes → seconds.
- Android unit tests must be run through the `resha overlay --monorepo` flow (a v0.87.1
  `facebook/react-native` checkout); historical local checkout: `/tmp/rn-checkout-0.87.1`.
- `node packages/react-native-resha/bin/resha.js` (not `bin/resha.js` at the repo root).
- Never request sandbox escalation; approval policy is "never".

---

## 8. Not done / open objectives

- **Fix the launch crash** (the blocker for everything below).
- Step 6 — C++ Native Animated direct adapter (+ skipping `AnimatedMountingOverrideDelegate`
  for direct surfaces). Dormant behind `cxxNativeAnimatedEnabled()`.
- Steps 5-iOS and 7 — impossible in this package (no Apple files).
- Lane tests (C++ and the Kotlin `UIThreadDirectViewApplierTest.kt` opcode map), Android native
  test run, `RNXAnimLane` profile evidence, doc updates.
- One clean single-line commit per design step; **no push** ever; npm publish explicitly out of
  scope.
- Known stale comment to fix: the paragraph above `classifyAnimatedPropKey` still lists `zIndex`
  as paint (it returns Layout now).
