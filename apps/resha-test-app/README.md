# Resha test app

A stock React Native **0.87.1** app (generated with
`@react-native-community/cli@20.2.0 init` from
`@react-native-community/template@0.87.1`) wired through the `react-native-resha`
integrations. It exists to prove the out-of-tree model works end to end:

- **Android** builds React Native from the materialized sources
  (`node_modules/.resha/react-native`) through a Gradle composite build plus
  dependency substitution, instead of the prebuilt `react-android` /
  `hermes-android` artifacts.
- **iOS** builds React-Core from the materialized sources, with
  `RCT_USE_PREBUILT_RNCORE=0` forced by `resha_pods.rb`.
- **Metro, Jest, autolinking and codegen** resolve `react-native` from that same
  tree, while `node_modules/react-native` stays pristine as the merge base that
  `resha validate` hashes against.

Everything under `node_modules/.resha/` is generated; never edit it by hand.

## Install

This directory is deliberately **not** a workspace member of the repo root (see
`.npmrc`): `resha materialize` and the integration helpers are written against
`<app>/node_modules`. Install from here:

```sh
npm install
```

`react-native-resha` is a `file:../../packages/react-native-resha` dependency, so
local edits to the package take effect immediately (npm symlinks it).

## Materialize and validate

```sh
npm run resha:materialize   # rebuild node_modules/.resha/react-native
npm run resha:validate      # check the overrides against node_modules/react-native
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
apply from: file("../node_modules/react-native-resha/integration/android/resha.settings.gradle")
extensions.configure(com.facebook.react.ReactSettingsExtension){ ex -> ex.autolinkLibrariesFromCommand() }
```

`android/app/build.gradle` then points codegen at the same tree:

```groovy
react {
  reactNativeDir = file("../../node_modules/.resha/react-native")
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
./gradlew :app:assembleDebug -PRN_UI_THREAD=true

# + the direct renderer
./gradlew :app:assembleDebug -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true

# + worker-owned commit preparation
./gradlew :app:assembleDebug -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true -PUI_THREAD_RENDERER_WORKER=true
```

Notes:

- `UI_THREAD_RENDERER` requires `RN_UI_THREAD`, and `UI_THREAD_RENDERER_WORKER`
  requires both. The build fails fast with a clear `GradleException` otherwise.
- Add `-PreactNativeArchitectures=arm64-v8a` to build a single ABI. A
  from-source React Native build compiles Hermes and all of ReactAndroid, so this
  is the difference between minutes and tens of minutes.
- `RN_PROFILE_UI_THREAD_RENDERER=true` turns on diagnostic timers.
- `RN_BUILD_CPP_TESTS=true` (needs `UI_THREAD_RENDERER=true` *and*
  `UI_THREAD_RENDERER_WORKER=true`, because the publication tests use the worker
  API) additionally configures the C++ `UIThreadRenderer` suites, using the
  GoogleTest sources that ship with the NDK. The target
  (`ui_thread_renderer_tests`) is attached to the `reactnative` target, so the
  build above produces it together with the app's native libraries:

  ```sh
  ./gradlew :app:assembleDebug \
    -PreactNativeArchitectures=arm64-v8a \
    -PRN_UI_THREAD=true -PUI_THREAD_RENDERER=true \
    -PUI_THREAD_RENDERER_WORKER=true -PRN_BUILD_CPP_TESTS=true

  RA=node_modules/.resha/react-native/ReactAndroid
  exe=$(find "$RA/build/intermediates/cxx" -type f -name ui_thread_renderer_tests | head -1)
  ```

  It is an Android binary, so run it on a device or emulator (`adb push` then
  `adb shell`), not on the host. AGP stages the executable and its native
  dependencies (`libjsi.so`, `libfbjni.so`) under
  `build/intermediates/cxx/<buildType>/<hash>/obj/<abi>/`, not next to
  `build.ninja`; push those, plus `libc++_shared.so` from the NDK, beside the
  executable.

  It is an Android binary, so run it on a device or emulator (`adb push` then
  `adb shell`), not on the host.

Confirm the switch reached the build:

```sh
grep -E "RN_UI_THREAD|UI_THREAD_RENDERER" \
  node_modules/.resha/react-native/ReactAndroid/build/generated/source/buildConfig/debug/com/facebook/react/BuildConfig.java
```

## iOS

`RN_UI_THREAD` is Android-only (on iOS the JS runtime already has its own
thread); the direct renderer is switched with environment variables.

```sh
cd ios
bundle install
bundle exec pod install

# off
xcodebuild -workspace ReshaTestApp.xcworkspace -scheme ReshaTestApp \
  -configuration Debug -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' build

# direct renderer
RCT_UI_THREAD_RENDERER_IOS=1 xcodebuild ... build

# + worker-owned commit preparation
RCT_UI_THREAD_RENDERER_IOS=1 RCT_UI_THREAD_RENDERER_WORKER_IOS=1 xcodebuild ... build
```

`RCT_UI_THREAD_RENDERER_WORKER_IOS=1` without `RCT_UI_THREAD_RENDERER_IOS=1`
aborts during `pod install`.

## Jest

```sh
npm test
```

`jest.config.js` spreads `reshaJestConfig(__dirname)`, which maps `react-native`
and its subpaths to the materialized tree and keeps its Flow sources
transformable, so the JS overrides (for example
`Libraries/LayoutAnimation/LayoutAnimation.js`) are what the tests exercise.
`__tests__/resha.test.js` asserts exactly that: it fails if `react-native`
resolves anywhere but `node_modules/.resha/react-native`.

## Layout

```
android/settings.gradle          applies integration/android/resha.settings.gradle
android/app/build.gradle         react { reactNativeDir = ../../node_modules/.resha/react-native }
android/gradle.properties        the Resha switches
ios/Podfile                      requires integration/ios/resha_pods
react-native.config.js           reactNativePath -> node_modules/.resha/react-native
metro.config.js                  withResha(...)
jest.config.js                   reshaJestModuleNameMapper(...)
```