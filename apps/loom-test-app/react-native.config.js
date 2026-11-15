/**
 * Loom: point the React Native CLI (autolinking, run-android, run-ios) at the
 * materialized tree instead of node_modules/react-native. `loom materialize`
 * must have run first; the Gradle and CocoaPods integrations do that for you,
 * and `yarn loom:materialize` does it on demand.
 *
 * Paths here are resolved relative to the app root (the directory holding this
 * file), not to the current working directory.
 */
const path = require('path');

// The example library (examples/loom-example-library) is linked by path: autolinking reads it from here
// and Metro resolves it through `watchFolders` and `extraNodeModules` (metro.config.js).
const exampleLibrary = {
  'react-native-loom-example-library': {
    root: path.resolve(__dirname, '../../examples/loom-example-library'),
  },
};

module.exports =
  // LOOM_BENCH_STOCK=1: the stock baseline build for the benchmarks (bench/run.js).
  process.env.LOOM_BENCH_STOCK === '1'
    ? {dependencies: exampleLibrary}
    : {reactNativePath: './node_modules/.loom/react-native', dependencies: exampleLibrary};
