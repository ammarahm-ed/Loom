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
const config = {};

const merged = mergeConfig(getDefaultConfig(__dirname), config);

// LOOM_BENCH_STOCK=1 bundles stock react-native: the benchmark baseline (bench/run.js).
module.exports =
  process.env.LOOM_BENCH_STOCK === '1' ? merged : withLoom(merged, __dirname);