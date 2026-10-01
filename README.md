# react-native-resha

Resha's UI-thread / direct-renderer work for React Native, maintained **out of
tree**: a package that layers explicit, hash-tracked, 3-way-mergeable overrides
on top of the **unmodified `react-native` from npm**, instead of a fork of
`facebook/react-native`.

Taking an upstream fix is then a version bump plus `resha upgrade`, not a fork
rebase.

## How it works

`packages/react-native-resha/overrides.json` lists every file Resha touches:

| type | meaning |
|---|---|
| `patch` | a full copy of an upstream file with Resha edits, derived from the pristine npm file whose sha256 is recorded as `baseHash` |
| `platform` | a Resha-only file that does not exist upstream |
| `delete` | an upstream file Resha removes (none today) |
| `test` | an Android unit test that npm does not ship; kept for CI, never materialized |

`resha materialize` compiles those into `node_modules/.resha/react-native`: a
copy of the pristine `node_modules/react-native` with the overrides applied,
plus a `.resha-stamp.json` that makes the step a no-op while it is current.
`node_modules/react-native` is never modified — it stays the 3-way merge base
and the tree `resha validate` hashes against.

Materialize refuses to run when validation fails: the installed RN version is
not the one the overrides target, an upstream file no longer matches its
`baseHash`, a `platform` file now also exists upstream, or `src/` has untracked
files.

## Requirements

- Node.js >= 20
- The app's `react-native` must be exactly the version the overrides target
  (`overrides.json > upstream.version`), which is also the package's pinned
  `peerDependency`.

## Install

```sh
yarn add react-native-resha
# or
npm install react-native-resha
```

RN can no longer come from prebuilt binaries on either platform: the overrides
patch React-Core and ReactAndroid sources, so RN always builds from source. That
is already true for the fork today.

## App integration

### Android

`android/settings.gradle`:

```groovy
apply from: file("../node_modules/react-native-resha/integration/android/resha.settings.gradle")
```

`android/settings.gradle.kts`:

```kotlin
apply(from = file("../node_modules/react-native-resha/integration/android/resha.settings.gradle"))
```

That runs `resha materialize`, includes the materialized tree as a composite
build, and substitutes `react-android` / `react-native` →
`:packages:react-native:ReactAndroid` and `hermes-android` / `hermes-engine` →
`:packages:react-native:ReactAndroid:hermes-engine`.

`android/app/build.gradle` — point the RN Gradle plugin's codegen at the same tree:

```groovy
react {
  reactNativeDir = file("../node_modules/.resha/react-native")
}
```

### iOS

`ios/Podfile`, replacing the usual `require Pod::Executable.execute_command(...)`
line:

```ruby
require_relative '../node_modules/react-native-resha/integration/ios/resha_pods'
require File.join(Resha.react_native_path(__dir__), 'scripts/react_native_pods')

# ...
use_react_native!(
  :path => Resha.react_native_path(__dir__),
  # ...existing options...
)
```

`Resha.react_native_path(__dir__)` materializes the tree and returns its path.
It also forces `RCT_USE_PREBUILT_RNCORE=0`, because prebuilt RN core binaries
cannot contain the overrides.

### Autolinking / React Native CLI

`react-native.config.js`:

```js
module.exports = {
  reactNativePath: './node_modules/.resha/react-native',
  // ...project/dependencies as usual...
};
```

### Metro

```js
const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');
const {withResha} = require('react-native-resha/integration/metro');

module.exports = withResha(
  mergeConfig(getDefaultConfig(__dirname), {
    // ...your config...
  }),
  __dirname,
);
```

`withResha` materializes if needed and maps `react-native` and `react-native/*`
to the materialized tree, so JS overrides such as `LayoutAnimation.js` are what
gets bundled.

### Jest

```js
const {reshaJestModuleNameMapper} = require('react-native-resha/integration/metro');

module.exports = {
  preset: 'react-native',
  moduleNameMapper: reshaJestModuleNameMapper(__dirname),
};
```

## Compile-time switches

Android (`android/gradle.properties`; `resha materialize` copies these into the
materialized tree, because an included Gradle build cannot see the app's
`gradle.properties`, and `-P` on the command line still wins):

| switch | effect |
|---|---|
| `RN_UI_THREAD` | RN's JavaScript runtime and native modules queue run on the UI thread |
| `UI_THREAD_RENDERER` | selects the independent direct JS-host → Yoga → Android view renderer (requires `RN_UI_THREAD`) |
| `UI_THREAD_RENDERER_WORKER` | worker-owned commit preparation, UI-thread mounting (requires `UI_THREAD_RENDERER`) |
| `RN_PROFILE_UI_THREAD_RENDERER` | diagnostic C++ and direct-mount Java timers (default off) |
| `RN_BUILD_CPP_TESTS` | builds the C++ `UIThreadRenderer` tests (requires `UI_THREAD_RENDERER`) |

iOS (environment variables, since `RN_UI_THREAD` is Android-only):

| variable | effect |
|---|---|
| `RCT_UI_THREAD_RENDERER_IOS=1` | selects the direct renderer |
| `RCT_UI_THREAD_RENDERER_WORKER_IOS=1` | worker-owned commit preparation (requires the above) |

## CLI

App commands (run from the app root):

```
resha materialize [--force] [--out <dir>]   build node_modules/.resha/react-native
resha validate                              check overrides against installed react-native
```

Maintainer commands (run in this repo):

```
resha status                                overrides by area, lines carried
resha diff [path-substring...]              unified diff of patches vs upstream
resha add <rn-relative-path> [--area a]     start overriding an upstream file
resha add --platform <rn-relative-path>     register a new Resha-only file
resha upgrade --to <version>                3-way merge overrides onto a new RN
resha overlay --monorepo <dir>              write overrides (+ Android tests, rn-tester
                                            benchmarks) into a react-native checkout
resha import-fork --fork <dir> --base <sha> capture a checkout's working tree back into
                                            overrides (inverse of overlay)
```

`--upstream <dir>` uses that react-native package instead of fetching one from npm.

`resha status` is the headline metric for shrinking the patch surface; see
[docs/plan.md](docs/plan.md) phase 2.

## Repository layout

```
package.json                     workspace root (workspaces: packages/*)
packages/react-native-resha/     the published package
  overrides.json                 upstream {package, version} + one entry per file
  src/                           full-file overrides, mirroring react-native package paths
  android-tests/                 ReactAndroid/src/test/** (npm does not ship these)
  bin/resha.js                   CLI
  lib/                           manifest, materialize, validate, upgrade, import-fork,
                                 overlay, status/diff
  integration/                   android/resha.settings.gradle, ios/resha_pods.rb, metro.js
apps/resha-test-app/             RN 0.87.1 app wired through the integrations
examples/rn-tester/              benchmark screens + NativeTextBenchmarkActivity patch
docs/                            plan and runbooks
```

`apps/resha-test-app` is deliberately **not** a workspace member: it keeps its own
`node_modules`, which is what `resha materialize` and the integration helpers
assume. Install it from its own directory (see its README).

## Developing against a react-native checkout

```sh
node packages/react-native-resha/bin/resha.js overlay --monorepo ~/Work/react-native
# ...hack in rn-tester...
node packages/react-native-resha/bin/resha.js import-fork --fork ~/Work/react-native --base v0.87.1
node packages/react-native-resha/bin/resha.js validate
```

`overlay` → `import-fork` is a lossless round trip on a clean checkout at the
upstream tag. See [docs/upgrading.md](docs/upgrading.md) for the upgrade runbook
and [docs/plan.md](docs/plan.md) for the migration plan.

## License

MIT. Files under `packages/react-native-resha/src/` are derived from
react-native (MIT, Meta Platforms, Inc. and affiliates); see
[LICENSE](LICENSE).