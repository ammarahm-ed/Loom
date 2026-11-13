# Getting started

::: info
Installation wires Loom into your app's native projects and builds React Native from source.
A one-command install (`npx react-native-loom init`), an Expo config plugin and prebuilt
binaries are coming.
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

`android/app/build.gradle`, pointing the React Native Gradle plugin at Loom's tree:

```groovy
react {
  reactNativeDir = file("../node_modules/.loom/react-native")
}
```

Then pick a configuration in `android/gradle.properties`:

```properties
# JavaScript on the main thread: instant updates
loom.renderer=loom
loom.js=main
loom.worker=true
```

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

Install pods with the Loom renderer switched on:

```sh
RCT_LOOM_RENDERER_IOS=1 RCT_LOOM_WORKER_IOS=1 bundle exec pod install
```

## Metro and the CLI

`metro.config.js`:

```js
const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');
const {withLoom} = require('react-native-loom/integration/metro');

module.exports = withLoom(mergeConfig(getDefaultConfig(__dirname), {}), __dirname);
```

`react-native.config.js`:

```js
module.exports = {reactNativePath: './node_modules/.loom/react-native'};
```

## Check it works

```sh
npx loom validate
```

Debug builds also print main-thread blocks longer than 50 ms to the console; see
[Finding main-thread jank](/guide/jank-reporter).
