# Getting started

::: info
Loom ships prebuilt: the Android and iOS cores are consumed from a Maven repository, so a first
build does not compile React Native. A source build (`loom.compileFromSource=true`) is still
available. A one-command install (`npx react-native-loom init`) and an Expo config plugin are
coming.
:::

## Requirements

- Node.js 20 or newer.
- Your app's `react-native` must be the React Native minor of your Loom release: Loom 0.87.x runs on
  React Native 0.87.x only (the peer dependency is `>=0.87.1 <0.88.0`). Support for a new
  React Native minor comes as a new Loom minor.
  - A React Native patch release that Loom has not verified yet still works, with a warning, as
    long as it did not change any file Loom replaces. If it did, `loom materialize` stops and asks
    you to update Loom.
  - Loom's own patch numbers are independent of React Native's: Loom 0.87.4 can run on React Native
    0.87.1.
- npm, Yarn (with `nodeLinker: node-modules`) or pnpm. Yarn Plug'n'Play is not supported, because
  React Native's native builds need a `node_modules` tree.
- If you patch `react-native` (patch-package, `pnpm patch`), patches to files Loom replaces have no
  effect; `loom materialize` lists them as warnings.

## Install

```sh
npm install react-native-loom
```

## Android

`android/settings.gradle`:

```groovy
apply from: file("../node_modules/react-native-loom/integration/android/loom.settings.gradle")
```

or, in `android/settings.gradle.kts`:

```kotlin
apply(from = file("../node_modules/react-native-loom/integration/android/loom.settings.gradle"))
```

That runs `loom materialize` and, by default, replaces `react-android` / `react-native` with Loom's
prebuilt `io.github.ammarahm-ed:react-android` from Maven Central (`LOOM_MAVEN_REPOSITORY`
replaces the repository, with a warning in the build log). The repository is added to
`dependencyResolutionManagement`, so it works with any `repositoriesMode`.
Hermes and everything else keep React Native's coordinates. With `loom.compileFromSource=true` it
includes the materialized tree as a composite build instead, and substitutes `react-android` /
`react-native` with `:packages:react-native:ReactAndroid` and `hermes-android` / `hermes-engine`
with `:packages:react-native:ReactAndroid:hermes-engine`.

`android/app/build.gradle`, pointing the React Native Gradle plugin at Loom's tree:

```groovy
react {
  reactNativeDir = file("../node_modules/.loom/react-native")
}
```

Then pick a configuration in `android/gradle.properties`. `loom materialize` copies these into the
materialized tree, because an included Gradle build cannot see the app's `gradle.properties`
(see [Configuration](/reference/configuration)):

```properties
# JavaScript on the main thread: instant updates
loom.renderer=true
loom.js=main
```

The four switches (`loom.renderer`, `loom.js`, `loom.compileFromSource`,
`loom.releaseProfiling`) are the same on iOS.

## iOS

`ios/Podfile`:

```ruby
require_relative '../node_modules/react-native-loom/integration/ios/loom_pods'
require File.join(Loom.react_native_path(__dir__), 'scripts/react_native_pods')

use_react_native!(
  :path => Loom.react_native_path(__dir__),
  # ...existing options...
)
```

`Loom.react_native_path(__dir__)` reads the switches (from `ios/Podfile.properties.json` or the
environment), materializes the tree and returns its path. With the defaults the app links Loom's
prebuilt core. With `loom.compileFromSource=true` it builds React Native from source, and with
`loom.renderer=false` it returns the stock package, so React Native's own artifacts are used.

```sh
bundle exec pod install
# or, for example:
LOOM_JS=main bundle exec pod install
```

Each switch also has an exportable environment name, for CI: `loom.renderer` is `LOOM_RENDERER`,
`loom.js` is `LOOM_JS`, `loom.compileFromSource` is `LOOM_COMPILE_FROM_SOURCE`, and so on
(`env loom.js=main pod install` works too). The environment wins over `Podfile.properties.json`.

## Metro and the CLI

`metro.config.js`:

```js
const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');
const {withLoom} = require('react-native-loom/integration/metro');

module.exports = withLoom(
  mergeConfig(getDefaultConfig(__dirname), {
    // ...your config...
  }),
  __dirname,
);
```

`withLoom` materializes if needed and maps `react-native` and `react-native/*` to the
materialized tree, so JavaScript overrides such as `LayoutAnimation.js` are what gets bundled.

`react-native.config.js`:

```js
module.exports = {reactNativePath: './node_modules/.loom/react-native'};
```

## Jest

`jest.config.js`:

```js
const {loomJestModuleNameMapper} = require('react-native-loom/integration/metro');

module.exports = {
  preset: 'react-native',
  moduleNameMapper: loomJestModuleNameMapper(__dirname),
};
```

## Check it works

```sh
npx loom validate
```

Debug builds also print main-thread blocks longer than 50 ms to the console; see
[Finding main-thread jank](/guide/jank-reporter).
