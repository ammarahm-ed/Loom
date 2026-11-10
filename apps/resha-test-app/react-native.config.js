/**
 * Resha: point the React Native CLI (autolinking, run-android, run-ios) at the
 * materialized tree instead of node_modules/react-native. `resha materialize`
 * must have run first; the Gradle and CocoaPods integrations do that for you,
 * and `yarn resha:materialize` does it on demand.
 *
 * Paths here are resolved relative to the app root (the directory holding this
 * file), not to the current working directory.
 */
module.exports =
  // RESHA_BENCH_STOCK=1: the stock baseline build for the benchmarks (bench/run.js).
  process.env.RESHA_BENCH_STOCK === '1'
    ? {}
    : {reactNativePath: './node_modules/.resha/react-native'};