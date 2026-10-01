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

module.exports = withResha(
  mergeConfig(getDefaultConfig(__dirname), config),
  __dirname,
);