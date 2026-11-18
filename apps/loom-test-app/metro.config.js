const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');
const {withLoom} = require('react-native-loom/integration/metro');

/**
 * Metro configuration
 * https://reactnative.dev/docs/metro
 *
 * `withLoom` materializes node_modules/.loom/react-native if needed and maps
 * every `react-native` / `react-native/*` import there, so JS overrides (for
 * example Libraries/LayoutAnimation/LayoutAnimation.js) are what gets bundled.
 *
 * @type {import('@react-native/metro-config').MetroConfig}
 */
const path = require('path');

// The example library lives outside the app: let Metro watch and resolve it.
const exampleLibrary = path.resolve(__dirname, '../../examples/loom-example-library');
const config = {
  // Setting watchFolders replaces the default, so list the app and the linked react-native-loom package too.
  watchFolders: [__dirname, path.resolve(__dirname, '../../packages/react-native-loom'), exampleLibrary],
  resolver: {
    extraNodeModules: {'react-native-loom-example-library': exampleLibrary},
    // The stock benchmark skips withLoom, so the example library's react-native imports need the app's node_modules.
    nodeModulesPaths: [path.join(__dirname, 'node_modules')],
  },
};

const merged = mergeConfig(getDefaultConfig(__dirname), config);

// LOOM_BENCH_STOCK=1 bundles stock react-native: the benchmark baseline (bench/run.js).
module.exports =
  process.env.LOOM_BENCH_STOCK === '1' ? merged : withLoom(merged, __dirname);