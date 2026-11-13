# Maintaining Loom

This page is for people changing Loom itself. To use Loom in an app, see
[Getting started](/guide/getting-started). For how the override files work, see
[Overrides model](/architecture/overrides); for the commands, [CLI](/reference/cli).

## Requirements

- Node.js 20 or newer.
- A `react-native` checkout at the upstream tag the overrides target, for the checkout workflow
  below.

## Repository layout

```
package.json                     workspace root (workspaces: packages/*)
packages/react-native-loom/      the published package
  overrides.json                 upstream {package, version} + one entry per file
  src/                           full-file overrides, mirroring react-native package paths
  android-tests/                 ReactAndroid/src/test/** (npm does not ship these)
  bin/loom.js                    CLI
  lib/                           manifest, materialize, validate, upgrade, import-fork,
                                 overlay, status/diff
  integration/                   android/loom.settings.gradle, ios/loom_pods.rb, metro.js
apps/loom-test-app/              React Native test app wired through the integrations, plus
                                 the benchmark harness (bench/)
examples/rn-tester/              benchmark screens + NativeTextBenchmarkActivity patch
brand/                           logo, lockups, social preview (generate.py)
website/                         this documentation site (VitePress)
.local/                          git-ignored scratch: notes, benchmark output
```

`apps/loom-test-app` is deliberately not a workspace member: it keeps its own `node_modules`,
which is what `loom materialize` and the integration helpers assume. Install it from its own
directory (see its README).

## Developing against a react-native checkout

```sh
node packages/react-native-loom/bin/loom.js overlay --monorepo ~/Work/react-native
# ...hack in rn-tester...
node packages/react-native-loom/bin/loom.js import-fork --fork ~/Work/react-native --base v0.89.0-rc.0
node packages/react-native-loom/bin/loom.js validate
```

`overlay` then `import-fork` is a lossless round trip on a clean checkout at the upstream tag.

## Licensing of the overrides

Files under `packages/react-native-loom/src/` are derived from React Native (MIT, Meta
Platforms, Inc. and affiliates); see the repository's `LICENSE`.
