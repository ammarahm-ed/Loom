const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');
const {withResha} = require('react-native-resha/integration/metro');

/**
 * Metro configuration
 * https://reactnative.dev/docs/metro
 *
 * `withResha` materializes node_modules/.resha/react-native if needed and maps
 * every `react-native` / `react-native/*` import there, so JS overrides (for
 * example Libraries/LayoutAnimation/LayoutAnimation.js) are what gets bundled.
 *
 * @type {import('@react-native/metro-config').MetroConfig}
 */
const config = {};

const merged = mergeConfig(getDefaultConfig(__dirname), config);

// RESHA_BENCH_STOCK=1 bundles stock react-native: the benchmark baseline (bench/run.js).
module.exports =
  process.env.RESHA_BENCH_STOCK === '1' ? merged : withResha(merged, __dirname);