# Phase 1 verification record

What was actually built and run to prove the out-of-tree model works, and what
each result was. Phase 1's contract is **no behaviour change**: the materialized
tree is the fork's `packages/react-native` plus the cruft fixes listed at the
bottom, and every build below uses that tree rather than the pristine
`node_modules/react-native`.

## Environment

| | |
|---|---|
| host | macOS, arm64, 12 cores / 24 GB |
| node | v22.20.0 |
| yarn / npm | 1.22.22 / 10.9.3 |
| JDK | Zulu 17.0.13 |
| Android SDK | platform android-37.0, build-tools 37.0.0, NDK 27.1.12297006 |
| Gradle | 9.4.1 (via the app wrapper) |
| Xcode | 26.6 / iOS Simulator 26.5 SDK |
| CocoaPods | 1.15.2 (bundler) |
| target | react-native 0.87.1, 89 overrides |

Local quirk worth knowing: `$ANDROID_HOME/ndk/27.2.12479018` is a symlink to
`27.1.12297006`, whose `package.xml` declares it as 27.2.12479018. Both NDK names
therefore resolve to the same physical NDK, which is why dropping the fork's
hardcoded `ndkVersion = "27.2.12479018"` in favour of upstream's
`libs.versions.ndkVersion` (27.1.12297006) changes nothing on this machine.

## Static checks

| check | result |
|---|---|
| `resha validate` (repo root and app) | `89 overrides valid` |
| `resha materialize` | `materialized react-native@0.87.1 + 84 overrides` (89 − 5 tests) |
| pristine tree untouched | `node_modules/react-native/Libraries/LayoutAnimation/LayoutAnimation.js` still hashes to the manifest `baseHash`; the platform files (`UIThreadRenderer.cpp`, …) exist only under `.resha` |
| autolinking | `npx react-native config` → `reactNativePath = <app>/node_modules/.resha/react-native` |
| codegen resolution | `@react-native/codegen` resolves to `<app>/node_modules/@react-native/codegen` from the materialized tree (Node walks up out of `.resha`) |
| Jest | 4 tests pass, including "react-native resolves to the materialized tree" and "an overridden JS module resolves to the materialized copy" |
| cold start | `rm -rf node_modules/.resha` then loading `metro.config.js` (i.e. `withResha`) re-materializes on demand — stamp written, Jest passes afterwards |
| guard: stray file | adding `src/__stray__.txt` fails validate with `src/__stray__.txt: not listed in overrides.json` |
| guard: drifted upstream | appending a line to a pristine file that a patch is based on fails validate with `upstream changed since override was based (baseHash mismatch)` |
| guard: platform collision | adding `UIThreadRenderer.cpp` upstream fails validate with `platform file now exists upstream; convert to patch or drop` |

## Android

`apps/resha-test-app`, `:app:assembleDebug`, `-PreactNativeArchitectures=arm64-v8a`,
React Native built from the materialized sources through the Gradle composite
build and the dependency substitution in `integration/android/resha.settings.gradle`.

| switches (`-P`) | build | APK | CMake cache (`RN_BUILD_CPP_TESTS`, `RN_UI_THREAD`, `UI_THREAD_RENDERER`, `WORKER`) |
|---|---|---|---|
| *(none — stock upstream)* | ✅ 3m55s | 62.8 MB | `OFF OFF OFF OFF` |
| `RN_UI_THREAD` | ✅ 1m17s | | `OFF ON OFF OFF` |
| `+ UI_THREAD_RENDERER` | ✅ 1m21s | 64.5 MB | `OFF ON ON OFF` |
| `+ UI_THREAD_RENDERER_WORKER`, `RN_BUILD_CPP_TESTS` | ✅ 1m21s | | `ON ON ON ON` |

Each combination gets its own CMake build directory, so the flags were verified in
the generated `CMakeCache.txt` rather than inferred from the command line. The
materialized `ReactAndroid/build/generated/source/buildConfig/debug/com/facebook/react/BuildConfig.java`
carries the same values for the Java/Kotlin half (`RN_UI_THREAD = true`, …).

The APK contains `libreactnative.so` (25.5 MB, from the materialized tree),
`libhermesvm.so`, `libjsi.so`, `libfbjni.so` and the app's own `libappmodules.so`.

`-P` switches reach the *included* build: the app's `android/gradle.properties`
keeps all five switches at `false`, and the values above come from the command
line (the same mechanism `materialize` documents when it copies the app's
switches into the materialized `gradle.properties`).

### C++ suites (on a device)

`ui_thread_renderer_tests` (GoogleTest from the NDK; the CMake target is
`EXCLUDE_FROM_ALL`, but `RN_BUILD_CPP_TESTS=true` attaches it to the `reactnative`
target, so the all-on app build produces it) was pushed to a connected arm64-v8a
device (API 30) together with `libjsi.so`, `libfbjni.so` and `libc++_shared.so`,
and run there:

```
[==========] 85 tests from 3 test suites ran. (2524 ms total)
[  PASSED  ] 85 tests.
```

68 `UIThreadRendererTest`, 7 `UIThreadRendererPublishTest`, 10
`UIThreadRendererWorkerTest`.

Note for anyone re-running this: the device already had an older
`ui_thread_renderer_tests` binary (dated 2026-09-29, built from the fork) in
`/data/local/tmp`. Running *that* binary reports one failure
(`UIThreadRendererPublishTest.ACloneOfAPublishedVersionIsPublishedAgain`) and
prints fork source paths. The freshly built binary, which embeds the materialized
paths, passes 85/85 — so push to a private directory and check the binary's
identity before drawing conclusions from a device run.

The same target was then built for **x86_64** inside the pristine checkout (the
configuration `.github/workflows/react-native-tests.yml` uses for the emulator
job), and that run caught two things the app builds had hidden:

- The suites need **all three** switches. `UIThreadRendererTest.cpp` carries no
  preprocessor guards and drives the publication API (`publishWorkerCommit`,
  `applyPublishedIdentities`, the submitted/mounted identity maps), which the
  binding only declares under `#if UI_THREAD_RENDERER_WORKER`
  (`UIThreadRendererBinding.h:280`). Building with only `RN_UI_THREAD` and
  `UI_THREAD_RENDERER` fails with 13 `no member named …` compile errors. The CMake
  test block now rejects that configuration up front, next to the existing
  `UI_THREAD_RENDERER` check, so the failure reads:

  ```
  RN_BUILD_CPP_TESTS=true requires UI_THREAD_RENDERER_WORKER=true:
  UIThreadRendererTest.cpp exercises the worker publication API.
  ```

  (`configureCMakeDebug` fails; the earlier run produced 26 `no member named`
  errors instead.)

- AGP stages the produced binary and its native dependencies under
  `build/intermediates/cxx/<buildType>/<hash>/obj/<abi>/`, **not** next to the
  `build.ninja` file in `.cxx`. A `find .cxx -name build.ninja` recipe can also
  pick one of CMake's `_CMakeLTOTest-*` probe directories. The docs and the
  workflow now locate the executable by name under `build/intermediates/cxx`,
  which is where `libjsi.so` and `libfbjni.so` sit as well — they have to be
  pushed with it, `libc++_shared.so` comes from the NDK.

With all three switches the run is `BUILD SUCCESSFUL in 1m 30s`, and
`build/intermediates/cxx/Debug/3n2b5027/obj/x86_64/ui_thread_renderer_tests` is an
`ELF 64-bit LSB pie executable, x86-64 … interpreter /system/bin/linker64`.

### JVM unit tests

`android-tests/**` is never materialized, and not only because npm does not
publish `ReactAndroid/src/test/**`: the five Resha test files depend on upstream
test helpers that live in that same unpublished source set. Copying just the
Resha files into the materialized tree does not compile:

```
e: .../fabric/mounting/UIThreadDirectViewApplierTest.kt:13:34 Unresolved reference 'ReactTestHelper'.
e: .../uimanager/BaseViewManagerTest.kt:21:21 Unresolved reference 'testutils'.
```

so these tests run in a full checkout, which is exactly what
`.github/workflows/react-native-tests.yml` does:

```sh
node packages/react-native-resha/bin/resha.js overlay --monorepo ~/Work/react-native
(cd ~/Work/react-native && \
  ./gradlew :packages:react-native:ReactAndroid:testDebugUnitTest \
    -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true)
```

The monorepo has a **single** `gradlew`, at the checkout root: `ReactAndroid` has
no wrapper of its own, and the root `settings.gradle.kts` is what includes
`:packages:react-native:ReactAndroid`.

Details worth keeping:

- the switches matter — the Android tests exercise the Resha code paths, so the run
  needs the same `-P` flags a device build uses;
- from an *app* build (composite build) the included build's projects are
  addressed with the build-name prefix:
  `:react-native:packages:react-native:ReactAndroid:testDebugUnitTest`, not
  `:packages:react-native:ReactAndroid:testDebugUnitTest`.
- if `ANDROID_NDK` is exported, the monorepo build fails before compiling
  anything: the root `build.gradle.kts` does
  `val ndkPath by extra(System.getenv("ANDROID_NDK"))` and `ReactAndroid` prefers
  that `ndkPath` over the catalog version, so a symlinked NDK trips AGP:

  ```
  [CXX1100] android.ndkVersion is [27.1.12297006] but android.ndkPath
  .../ndk/27.1.12297006 refers to a different version [27.2.12479018].
  ```

  Unsetting `ANDROID_NDK` for the run restores the catalog NDK, which is what CI
  uses. (App builds never hit this: an app's root build script defines no
  `ndkPath`.)

### Result

```
86 classes, 549 tests, 0 failures, 0 errors
BUILD SUCCESSFUL
```

(Gradle only prints a test count when something fails, so the totals come from the
JUnit XML under `build/test-results/testDebugUnitTest`.) Overlaying
`android-tests/**` on a pristine `v0.87.1` checkout is enough to run the Resha
tests: nothing has to be copied out of upstream by hand.

That first run was **not** green, and the failure was in the tests rather than in
the applier. `UIThreadDirectViewApplierTest.viewManagerFailure_invalidatesSurfaceAndDoesNotReplayBatch`
and `…propsAndStateFailuresReportTheExactFailingOperation` failed with
`java.lang.AssertionError: Expecting code to raise a throwable`. The three
`assertThatThrownBy { applier.applyBatch(...) }.isInstanceOf(...)` expectations in
that file predate the fork's newest applier change, which replaced the
`throw failure` at the end of the catch block with the documented "the failure is
consumed here" contract — rethrowing would send a Java exception across JNI into
the renderer's effect loop. That is the same change that moved `surfaceGeneration`
to a `ConcurrentHashMap`, stopped copying the deferred batch arrays and let
`OP_CREATE` retire a retained view state; `import-fork` shipped the newer
implementation together with the older expectations. The expectations now assert
`assertThatCode { … }.doesNotThrowAnyException()`, and the rest of both tests —
the reported diagnostic, the tag invalidation, "no command is replayed" — is
untouched. Re-run: 16/16 in that class, 549/549 overall, which also rules out the
alternative explanation (a deferred batch that never injects the failure) because
those follow-up assertions pass.

### App runtime on a device (smoke test)

The matrix above proves the switches *compile*; whether the direct renderer
mounts, commits and tears down is a different question, so the all-on APK was
installed on the same connected arm64-v8a device (API 30) and exercised. The
screen is `apps/resha-test-app/ReshaSmokeTest.tsx`, rendered by `App.tsx`: a
counter, a 120-step `requestAnimationFrame` burst, a 60 Hz `setInterval` ticker, a
native-driver `Animated.loop`, a 120-row `FlatList` of pressable rows and a list
remount, each logging a `[resha-smoke]` line so a `logcat` capture says how far it
got. That the direct path actually ran is the one thing `logcat` can prove, since
ART only JIT-compiles a method that has executed:

```
I om.reshatestap: Compiler allocated 7347KB to compile void
  com.facebook.react.fabric.mounting.UIThreadDirectViewApplier.applyBatchNowInternal(int,
  com.facebook.react.fabric.mounting.SurfaceMountingManager, int[], java.lang.Object[], …)
```

Running it found three defects that no build could: two in the Metro integration,
one in the binding.

**1. Export-only subpaths did not resolve.** `withResha` rewrote
`react-native/<subpath>` into the literal path `<rnDir>/<subpath>`, but
`asset-registry`, `setup-env` and `react-private-interface` have no file at the
package root — they exist only as `exports` map entries. Metro answered
`UnableToResolveError … targetModuleName: react-native/asset-registry` and the app
came up on a redbox; `npx react-native bundle` reproduces it without a device
(exit 1 before the fix). `integration/metro.js` now looks the subpath up in the
package's own `exports` map first (`exportsSubpath`) and falls back to the literal
path, which is what the real `Libraries/**` files need. After the fix the bundle
exits 0 at 4,109,321 bytes, with 604 modules under
`node_modules/.resha/react-native/` and none under the pristine
`node_modules/react-native/Libraries`. The obvious alternative — adding the
materialized tree to `resolver.nodeModulesPaths` — is wrong and stays wrong:
`metro-resolver` walks up from the origin module before it consults that list, so
the pristine copy would have won the lookup silently.

**2. The pre-main module was dropped.** With resolution fixed the app started but
threw `ReferenceError: Property 'window' doesn't exist` from `AppContainer-dev.js`.
`@react-native/metro-config` sets
`getModulesRunBeforeMainModule: () => [require.resolve('react-native/setup-env')]`,
and Node resolves that against the *pristine* package while every module in the
graph now comes from `rnDir`. Metro's `getAppendScripts` emits a `__r(…)` for an
entry only when a graph module's path equals it exactly, and drops the rest
without a warning, so `setup-env` never ran — and neither did the environment it
installs (`window`, the timers, the console, the HMR client). `withResha` now
wraps the serializer and retargets any `node_modules/react-native/` path into the
materialized tree; the bundle tail gained `__r(111)` for
`node_modules/.resha/react-native/src/setup-env.js`, the same shape a control
bundle built from the pristine config emits for the pristine path. Two Metro
behaviours found only by running: exact-match pre-main entries, and a resolver
order that silently prefers the wrong copy.

**3. `stageNativeProps` used a moved-from token.** The first native-driver
animation update killed the process: `signal 11 (SIGSEGV), code 1 (SEGV_MAPERR),
fault addr 0x0`, frame `UIThreadRendererBinding::stageNativeProps(…)+676`
(`BuildId 5265e16a…`), which `llvm-addr2line` resolves to
`UIThreadRendererBinding.cpp:429`. `submitWorkerOperation` takes the token by
value, so the `std::move(token)` above it left the local empty while the completion
callback still read `token->surfaceId`; the fault address is `0x0` because
`SurfaceToken::surfaceId` is the first member. The path is `Animated` native driver
→ `FabricUIManager.synchronouslyUpdateViewOnUIThread` →
`FabricUIManagerBinding::recordDirectNativeProps` → `recordNativeProps`, so any
native-driver animation on a mounted view hits it. The surface id is now read
before the move. The fork's copy of this file is byte-identical in that region, so
the fork crashes the same way; a sweep of the remaining `std::move(token)` site
(`submitWorkerJob`, which captures copies first) found no second instance.

After the fix (`BuildId 2ec09dfa…`) the app cold-starts and stays up through the
burst's 120 commits, the 60 Hz ticker, the list remount and touch input: rows
counting their own taps, `press +1` and `press row N at x,y` lines appearing in
`logcat` for injected taps, the animated box still moving, and no crash or `E/`
line in `logcat`. The burst takes 22.2 s for its 120 commits in this debug/dev
build; the switches-off control in the next section times the same burst at
14.1 s, so on this device the direct path does cost JS throughput.

### Switch matrix on a device (all-off vs all-on)

Fabric is not part of that matrix, because React Native 0.87 no longer lets an app
turn it off. `ReactRootProjectPlugin` runs `checkLegacyArchProperty` and then sets
`newArchEnabled`/`react.newArchEnabled` to `true` on every subproject ("We set the
New Architecture properties to true for all subprojects…"), warning that
"Setting `newArchEnabled=false` in your `gradle.properties` file is not supported
anymore since React Native 0.82 … The application will run with the New
Architecture enabled by default"; iOS says the same from `warn_if_new_arch_disabled`
in `scripts/react_native_pods.rb`. A build with every Resha switch off still
reports `"fabric":true` at startup, so the only axis an app can choose is the Resha
trio.

Each APK now says which side of that matrix it is on. `MainApplication` registers
`ReshaBuildConfigPackage`, whose module answers `getSwitches()` from
`com.facebook.react.BuildConfig` (the generated ReactAndroid one) with
`rnUiThread`/`uiThreadRenderer`/`uiThreadRendererWorker`/`profile`; the smoke screen
renders them as a banner and logs them at mount. It reads the compiled-in values on
purpose: the all-on build passes the switches as `-P` flags, so
`gradle.properties` never reflects them, and only the compiled `BuildConfig` knows
what the installed binary actually is.

|  | all off | all on |
| --- | --- | --- |
| switches | `-P…=false` (gradle.properties default) | `-PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true` |
| APK | 62,821,929 B | 65,155,033 B |
| `libreactnative.so` | 23,744,888 B, `18a96885…` | 26,077,992 B, `2ec09dfa…` |
| CMake dir | `.cxx/Debug/2k6k5q13` OFF/OFF/OFF | `.cxx/Debug/6w2c6q30` ON/ON/ON |
| banner | `UI THREAD OFF / RENDERER OFF / WORKER OFF` | `UI THREAD ON / RENDERER ON / WORKER ON` |
| JS thread | `ReactNativeJS` pid 13054 tid 13112 | pid 11698 tid 11698 |
| direct applier | absent from `logcat` | `applyBatchNowInternal` JIT-compiled |
| burst, 120 commits | 14.1 s | 22.0–22.4 s |
| crashes | none | none |

Both builds mount, render the 120-row list, take injected taps and stay up; the
difference is throughput, not correctness. On this device the all-on burst is ~1.6×
slower because JS shares the UI thread with the committing — the trade the
architecture makes for rendering directly. One sample for all-off and four for
all-on, debug/dev bundle, one API 30 device: a baseline, not a benchmark.

Injected input needs care, and the smoke screen now logs every press so a tap can
be checked without reading pixels. `adb shell input tap X Y` takes physical pixels
and is delivered 1:1; what JS sees in `nativeEvent.pageX/pageY` is dp, i.e.
physical ÷ (density/160) = ÷2.625 at 420 dpi. Three taps confirm it: `540 1300` →
`press row 1 at 206,495`, `540 1900` → `press row 7 at 206,724`, `300 1600` →
`press row 4 at 114,610` (1300/2.625 = 495.2, 300/2.625 = 114.3). `uiautomator dump`
is not usable on this screen: the running native-driver animation means the tool
never sees an idle window, and it returns a stale hierarchy from whatever app
dumped last.

## iOS

`apps/resha-test-app/ios`, `pod install` + `xcodebuild -workspace
ReshaTestApp.xcworkspace -scheme ReshaTestApp -configuration Debug -sdk
iphonesimulator -destination 'generic/platform=iOS Simulator' ARCHS=arm64
ONLY_ACTIVE_ARCH=YES`.

| configuration | pod install | build |
|---|---|---|
| *(none)* | ✅ | ✅ BUILD SUCCEEDED (717 compiles) |
| `RCT_UI_THREAD_RENDERER_IOS=1` | ✅ | ✅ BUILD SUCCEEDED (224 compiles) |
| `+ RCT_UI_THREAD_RENDERER_WORKER_IOS=1` | ✅ | ✅ BUILD SUCCEEDED |

Evidence that this is a source build of the materialized tree, not a prebuilt
React Native:

- `Podfile.lock` contains 151 references to `.resha/react-native` and **zero**
  occurrences of "prebuilt";
- 146 `Pods/Target Support Files/**` files reference the materialized tree;
- build logs compile `.../node_modules/.resha/react-native/ReactCommon/react/nativemodule/core/ReactCommon/TurboModuleBinding.cpp`
  and friends;
- `integration/ios/resha_pods.rb` forces `RCT_USE_PREBUILT_RNCORE=0` (the switch
  that would otherwise install the prebuilt React-Core facades).

`pod install` also writes `ios/Podfile.lock`, the workspace and the standard
CocoaPods integration into `ReshaTestApp.xcodeproj` / `Info.plist`
(`RCTNewArchEnabled`); those are committed so a fresh clone can build after
`pod install`.

The iOS switches are read by the podspecs at **pod install** time (the flags are
baked into the generated Pods project), so each combination needs its own
`pod install` — not just a new `xcodebuild`.

## Import/overlay round trip

- `resha import-fork --fork ~/Work/resha --base a59eff64fa --version 0.87.1`
  imported 90 overrides and re-captured `examples/rn-tester/rn-tester.patch`
  **byte-identically**; the 5 `.bak` scratch files in the fork were skipped.
- After re-applying the two documented cruft fixes, the package tree is
  byte-identical to the commit: `git status` reports no changes under
  `packages/`. In other words the capture is in sync with the fork, and the only
  deltas are intentional:

| file | delta vs fork |
|---|---|
| `ReactAndroid/build.gradle.kts` | hardcoded `ndkVersion` replaced by upstream's `rootProject`/catalog logic; `codegenDir` resolved through `findNodeModulePath` |
| `ReactAndroid/hermes-engine/build.gradle.kts` | dropped entirely — after the fix above it is byte-identical to upstream |

- A file-by-file comparison against the fork's current working tree found exactly
  those two deltas and nothing else (95 changed files in the fork = 89 overrides +
  the dropped hermes file + 5 scratch files).

Re-verified after the final import, from a pristine checkout: `resha overlay`
into a fresh `v0.87.1` clone (`git clone --depth 1 --branch v0.87.1`, 109 MB)
writes all 89 overrides and applies the benchmark patch cleanly — 20 rn-tester
files, 12 modified and 8 added, on a tree with no other changes — and
`import-fork` back out of that checkout reproduces
`packages/react-native-resha/src/**` **byte-identically**.

`rn-tester.patch` capture is deterministic as of that check. `git diff`
abbreviates the blob hashes in its `index` lines according to how many objects
the repository holds (10 characters in the fork, 7 in a shallow clone), so the
same working tree produced different patch *files*; `captureRnTester` now passes
`--full-index`, and two consecutive captures are byte-identical.

## Not verified locally

- The **emulator** leg of the C++ job: the x86_64 executable was built and checked
  as an Android binary locally, but it was not run on an emulator (the device run
  above is arm64-v8a, where the suites pass 85/85).
- The GitHub Actions workflows themselves have not been executed (no CI runs
  available from this machine).
- Runtime behaviour is verified on **Android debug builds only** (the smoke test
  above, run all-on and all-off). Nothing was run in a release build or on iOS, no
  regression test covers the three runtime defects found that way — the
  `stageNativeProps` move in particular is still only guarded by the fix itself —
  and the all-off/all-on throughput comparison rests on a single all-off sample.

## Animation fast lane launch crash fix

The all-on smoke app failed on its first Native Animated transform update. The
instrumented device build logged `clsfy init paintObj=0 layoutObj=0`, a valid
`transform` array, then `TypeError: expected dynamic type 'object', but had type
'null'`. The lane had introduced `facebook::react::AnimatedProps`, already defined
by upstream `ReactCommon/react/renderer/animationbackend/AnimatedProps.h` with a
completely different layout. This ODR violation allowed the backend constructor
to initialize the lane object incorrectly.

The lane type is now `ClassifiedAnimatedProps`; its tests include both headers in
one translation unit. Diagnostic logs were removed. `zIndex` uses the layout path
because draw-order changes require a mounting diff. The earlier overlay object
initialization fix is retained.

Build and native suite verification:

```sh
cd apps/resha-test-app/android
env -u ANDROID_NDK ./gradlew :app:assembleDebug \
  -PreactNativeArchitectures=arm64-v8a \
  -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true \
  -PUI_THREAD_RENDERER_WORKER=true -PRN_BUILD_CPP_TESTS=true
```

- Build passed. The installed APK's banner reports all three switches `true`.
- Fresh `ui_thread_renderer_tests` from
  `ReactAndroid/build/intermediates/cxx/Debug/143b4oi6/obj/arm64-v8a` was pushed with
  `libjsi.so`, `libfbjni.so`, and `libc++_shared.so` to
  `/data/local/tmp/resha-launch-fix` on Sony `QV7120NC26` (API 30).
- Device execution with `LD_LIBRARY_PATH=.` passed **87/87 tests** (2.6 seconds),
  including native-driver transform classification and mixed paint/layout/text
  payloads, nested style, null style, non-object inputs, and non-string keys.
- Initial launch plus three force-stop/relaunch cycles mounted successfully.
  The looping native transform remained active; screenshots captured different
  positions. The 60 Hz ticker, 120-step burst, and list remount also ran without
  a null-object exception or React host teardown. This is a stability check, not
  a frame-rate or latency benchmark.
- `resha validate` reports **92 overrides valid**; `git diff --check` passes.

Logs: `/tmp/resha-crash-diagnostic-device.log`,
`/tmp/animlane-crash-fixed-build.log`, `/tmp/resha-launch-fix-cpp-tests.log`,
`/tmp/resha-launch-fixed-device.log`.

During this verification, materialization was found to delete the generated
Android build trees on every override change, forcing repeated dependency
downloads. It now retains `.gradle`, the root `build`, `ReactAndroid/build`,
`ReactAndroid/.cxx`, and Hermes's `build`/`.cxx` directories by moving them into the
replacement tree. This also applies to `--force`; source overrides are still
refreshed from pristine upstream. A private temporary-tree integration test
checks ordinary refresh and `--force`, asserting that cached files keep their
contents/inodes and stale source files disappear:

```sh
node --test packages/react-native-resha/tests/materialize.test.js
```

Result: **1/1 passed**. No push or publication was performed. This verifies the
launch fix, not the full latency/scale/precedence matrix in the animation design.

### iOS Simulator verification of the launch fix

Built and ran the same corrected sources on the already booted **iPhone 17 Pro,
iOS 26.5**, Simulator UDID `27F1F2F3-B71A-41BA-81D2-D13A46701B5B`:

```sh
cd apps/resha-test-app/ios
RCT_UI_THREAD_RENDERER_IOS=1 RCT_UI_THREAD_RENDERER_WORKER_IOS=1 bundle exec pod install
xcodebuild -workspace ReshaTestApp.xcworkspace -scheme ReshaTestApp \
  -configuration Debug -sdk iphonesimulator \
  -destination 'id=27F1F2F3-B71A-41BA-81D2-D13A46701B5B' \
  -derivedDataPath build ARCHS=arm64 ONLY_ACTIVE_ARCH=YES
```

- **BUILD SUCCEEDED**. Existing build folders and dependency caches were reused.
- Generated React-Fabric and React-RCTFabric configurations contain
  `UI_THREAD_RENDERER=1 UI_THREAD_RENDERER_WORKER=1`; React-RuntimeApple contains
  `UI_THREAD_RENDERER_IOS=1`. The smoke screen's switch-reporting module is
  Android-only, so its iOS banner says switches unavailable.
- LLDB attached to the running Simulator process and hit
  `classifyAnimatedProps` on `com.apple.main-thread`. The captured backtrace is
  `RCTNativeAnimatedNodesManager` → `RCTPropsAnimatedNode.updateView` →
  `RCTSurfacePresenter` → `UIThreadRendererBinding::recordNativeProps` → the
  classifier using `ClassifiedAnimatedProps`. The debugger was then detached.
  This confirms the affected native animation path was exercised at runtime.
- The looping native transform visibly moved between screenshots. Increment,
  the ticker (observed at 478 and 1327 ticks), the burst (count reached 120), and
  list remount were exercised while the animation continued. The process stayed
  alive and the smoke screen remained visible.
- Initial launch plus **three terminate/relaunch cycles** succeeded. Each new
  process was checked alive after five seconds; the final smoke screen remained
  visible with the animation running. No ReshaTestApp crash report was produced.
- No additional C++ or Objective-C changes were needed for this launch fix.
  CocoaPods updated the five switch-dependent local podspec checksums in
  `Podfile.lock`; dependency versions did not change.

Evidence: `/tmp/resha-ios-crash-pods.log`, `/tmp/resha-ios-crash-build.log`,
`/tmp/resha-ios-classifier-lldb.log`, `/tmp/resha-ios-launch-unified.log`.
This verifies Simulator launch and smoke behavior on the native-animation path;
it does not establish the full iOS fast-lane latency/scale/precedence matrix or
physical-device behavior.
