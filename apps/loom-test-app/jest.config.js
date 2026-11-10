const {loomJestConfig} = require('react-native-loom/integration/metro');

module.exports = {
  preset: '@react-native/jest-preset',
  // Loom: resolve `react-native` against the materialized tree (and keep its Flow
  // sources transformable), so tests exercise the overridden JS exactly like Metro
  // does at runtime.
  ...loomJestConfig(__dirname),
};