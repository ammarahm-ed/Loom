<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="brand/loom-icon-dark.svg">
  <img src="brand/loom-icon-light.svg" alt="Loom" width="120" height="120">
</picture>

# Loom

**A faster renderer for React Native**

<a href="website/guide/what-is-loom.md">Documentation</a> ·
<a href="website/guide/getting-started.md">Getting started</a> ·
<a href="website/guide/compatibility.md">Supported libraries</a> ·
<a href="website/benchmarks.md">Benchmarks</a>

</div>

<!-- The documentation links point at the docs sources in this repository. They move to the docs site URL once it is deployed. -->

## Overview

Loom replaces Fabric's renderer with its own: a retained node tree, layout on a worker thread, and one batch of typed view operations per commit. JavaScript runs on the main thread, like a native app, or on its own thread, chosen by a build switch. React code, components and libraries work unchanged.

Loom is not a fork. It is a package of overrides applied to the unmodified `react-native` from npm.

## Highlights

- **Updates in the same frame.** With JavaScript on the main thread, a tap reaches your handler during Android's input handling and the result is drawn in the frame a native app would draw it in.
- **Your libraries, unchanged.** Loom presents Fabric's C++ contracts on its own tree, so libraries work without changes, Reanimated's layout animations and shared element transitions included.
- **Your choice of thread.** JavaScript on the main thread, or on its own thread so heavy JavaScript never blocks a frame.
- **Debug-time jank reports.** Debug builds report main-thread blocks longer than 50 ms, with the cause and sampled JavaScript stacks mapped to your source.
- **Not a fork.** Hash-tracked overrides on top of stock `react-native`; taking a React Native release is a version bump plus `loom upgrade`.

## Install

```sh
npm install react-native-loom
```

See [Getting started](website/guide/getting-started.md) for the Android, iOS, Metro and Jest setup.

Loom targets React Native **0.89.0-rc.0**, the exact version it pins as a peer dependency.

## Status

Pre-release.

## License

MIT, see [LICENSE](LICENSE). Files under `packages/react-native-loom/src/` are derived from React Native (MIT, Meta Platforms, Inc. and affiliates).

Loom is not affiliated with Meta. React Native is a trademark of Meta Platforms, Inc.

<!-- Repository: https://github.com/ammarahm-ed/Loom -->
