# Loom test app

A stock React Native **0.87.1** app (generated with
`@react-native-community/cli@20.2.0 init` from
`@react-native-community/template@0.87.1`) wired through the Loom
integrations. It exists to prove the out-of-tree model works end to end:

- **Android** builds React Native from the materialized sources
  (`node_modules/.loom/react-native`) through a Gradle composite build plus
  dependency substitution, instead of the prebuilt `react-android` /
  `hermes-android` artifacts.
- **iOS** builds React-Core from the materialized sources, with
  `RCT_USE_PREBUILT_RNCORE=0` forced by `loom_pods.rb`.
- **Metro, Jest, autolinking and codegen** resolve `react-native` from that same
  tree, while `node_modules/react-native` stays pristine as the merge base that
  `loom validate` hashes against.

Everything under `node_modules/.loom/` is generated; never edit it by hand.

## Install

This directory is deliberately **not** a workspace member of the repo root (see
`.npmrc`): `loom materialize` and the integration helpers are written against
`<app>/node_modules`. Install from here:

```sh
npm install
```

`react-native-loom` is a `file:../../packages/react-native-loom` dependency, so
local edits to the package take effect immediately (npm symlinks it).

## Materialize and validate

```sh
npm run loom:materialize   # rebuild node_modules/.loom/react-native
npm run loom:validate      # check the overrides against node_modules/react-native
```

The Gradle and CocoaPods integrations run `materialize` for you, and it is a
no-op while the stamp is current.

## Android

`android/settings.gradle` applies the integration. `apply from:` must come after
the `plugins {}` block (Gradle 9 allows nothing else before it) and before
`autolinkLibrariesFromCommand()`, which resolves `react-native.config.js`'s
`reactNativePath` and so needs the materialized tree to exist:

```groovy
pluginManagement { includeBuild("../node_modules/@react-native/gradle-plugin") }
plugins { id("com.facebook.react.settings") }
apply from: file("../node_modules/react-native-loom/integration/android/loom.settings.gradle")
extensions.configure(com.facebook.react.ReactSettingsExtension){ ex -> ex.autolinkLibrariesFromCommand() }
```

`android/app/build.gradle` then points codegen at the same tree:

```groovy
react {
  reactNativeDir = file("../../node_modules/.loom/react-native")
}
```

The switches live in `android/gradle.properties`; all of them are `false` by
default, which is stock upstream React Native. They are read by the materialized
ReactAndroid build, and `-P<NAME>=true` on the command line wins.

```sh
cd android

# off
./gradlew :app:assembleDebug

# the UI-thread runtime only
./gradlew :app:assembleDebug -Ploom.js=main

# + the Loom renderer
./gradlew :app:assembleDebug -Ploom.js=main -Ploom.renderer=loom

# + worker-owned commit preparation
./gradlew :app:assembleDebug -Ploom.js=main -Ploom.renderer=loom
```

Notes:

- `LOOM_RENDERER` requires `LOOM_JS_ON_MAIN`, and `LOOM_WORKER`
  requires both. The build fails fast with a clear `GradleException` otherwise.
- Add `-PreactNativeArchitectures=arm64-v8a` to build a single ABI. A
  from-source React Native build compiles Hermes and all of ReactAndroid, so this
  is the difference between minutes and tens of minutes.
- `LOOM_PROFILE=true` turns on diagnostic timers.
- `LOOM_BUILD_CPP_TESTS=true` (needs `LOOM_RENDERER=true` *and*
  `LOOM_WORKER=true`, because the publication tests use the worker
  API) additionally configures the C++ `LoomRenderer` suites, using the
  GoogleTest sources that ship with the NDK. The target
  (`ui_thread_renderer_tests`) is attached to the `reactnative` target, so the
  build above produces it together with the app's native libraries:

  ```sh
  ./gradlew :app:assembleDebug \
    -PreactNativeArchitectures=arm64-v8a \
    -Ploom.js=main -Ploom.renderer=loom \
    -Ploom.cppTests=true

  RA=node_modules/.loom/react-native/ReactAndroid
  exe=$(find "$RA/build/intermediates/cxx" -type f -name ui_thread_renderer_tests | head -1)
  ```

  It is an Android binary, so run it on a device or emulator (`adb push` then
  `adb shell`), not on the host. AGP stages the executable and its native
  dependencies (`libjsi.so`, `libfbjni.so`) under
  `build/intermediates/cxx/<buildType>/<hash>/obj/<abi>/`, not next to
  `build.ninja`; push those, plus `libc++_shared.so` from the NDK, beside the
  executable.

Confirm the switch reached the build:

```sh
grep -E "LOOM_JS_ON_MAIN|LOOM_RENDERER" \
  node_modules/.loom/react-native/ReactAndroid/build/generated/source/buildConfig/debug/com/facebook/react/BuildConfig.java
```

## iOS

`LOOM_JS_ON_MAIN` is Android-only (on iOS the JS runtime already has its own
thread); the Loom renderer is switched with environment variables.

```sh
cd ios
bundle install
bundle exec pod install

# off
xcodebuild -workspace LoomTestApp.xcworkspace -scheme LoomTestApp \
  -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' build

# Loom renderer
RCT_LOOM_RENDERER_IOS=1 xcodebuild ... build

# + worker-owned commit preparation
RCT_LOOM_RENDERER_IOS=1 xcodebuild ... build
```

The worker is always part of the Loom renderer; there is no separate switch.

## Smoke test

`App.tsx` renders `LoomSmokeTest`, which is meant to be exercised on a device.
It puts the three switches under load — a 120-step `requestAnimationFrame` burst,
a 60 Hz `setInterval` ticker, a native-driver `Animated.loop`, a 120-row
`FlatList` that can be remounted, and per-row `Pressable`s counting their own
taps — and logs everything with a `[loom-smoke]` prefix.

Its banner reads the switches from `com.facebook.react.BuildConfig`, through the
`LoomBuildConfig` native module (`LoomBuildConfigModule.kt` /
`LoomBuildConfigPackage.kt`, registered in `MainApplication.kt`): green when all
three are on, grey when all are off, amber when mixed, with the raw values
underneath and the same line in `logcat` at mount. It deliberately does not read
`gradle.properties` — the all-on build passes the switches as `-P` flags, so only
the compiled-in values describe the binary that is actually installed.

```sh
# build one combo, install it, and let the banner say which one it is
cd android
./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a \
  -Ploom.js=main -Ploom.renderer=loom
adb install -r -t app/build/outputs/apk/debug/app-debug.apk
```

`adb shell input tap X Y` takes **physical** pixels and delivers them 1:1, while
`nativeEvent.pageX/pageY` in JS are dp, so multiply by `density/160` (2.625 at
420 dpi) to line the two up. Every press logs (`press +1`, `press row N at x,y`),
so an injected tap can be verified from `logcat` alone:

```sh
adb logcat -c
adb shell am start -n com.loomtestapp/.MainActivity
adb logcat -d | grep -a loom-smoke     # mounted, build switches …, press …, burst …
```

`uiautomator dump` is not useful on this screen: the running native-driver
animation keeps the window busy, so it returns a stale hierarchy from another app.

## Jest

```sh
npm test
```

`jest.config.js` spreads `loomJestConfig(__dirname)`, which maps `react-native`
and its subpaths to the materialized tree and keeps its Flow sources
transformable, so the JS overrides (for example
`Libraries/LayoutAnimation/LayoutAnimation.js`) are what the tests exercise.
`__tests__/loom.test.js` asserts exactly that: it fails if `react-native`
resolves anywhere but `node_modules/.loom/react-native`.

## Layout

```
android/settings.gradle          applies integration/android/loom.settings.gradle
android/app/build.gradle         react { reactNativeDir = ../../node_modules/.loom/react-native }
android/gradle.properties        the Loom switches
ios/Podfile                      requires integration/ios/loom_pods
react-native.config.js           reactNativePath -> node_modules/.loom/react-native
metro.config.js                  withLoom(...)
jest.config.js                   loomJestModuleNameMapper(...)
```