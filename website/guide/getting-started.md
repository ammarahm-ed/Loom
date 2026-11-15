# Getting started

::: info
Loom ships prebuilt: the Android and iOS cores are consumed from a Maven repository, so a first
build does not compile React Native. A source build (`loom.compileFromSource=true`) is still
available. A one-command install (`npx react-native-loom init`) and an Expo config plugin are
coming.
:::

## Requirements

- Node.js 20 or newer.
- Your app's `react-native` must be exactly the version Loom targets (`0.89.0-rc.0`),
  which is also the package's pinned peer dependency.

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
prebuilt `io.github.ammarahm-ed:react-android` (the repository comes from `LOOM_MAVEN_REPOSITORY`).
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

The five switches (`loom.renderer`, `loom.js`, `loom.worker`, `loom.compileFromSource`,
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
env loom.js=main bundle exec pod install
```

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
