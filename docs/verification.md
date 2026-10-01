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
- Runtime behaviour on device/simulator beyond "it builds and links": the APK was
  not installed and the app was not launched.