const {reshaJestConfig} = require('react-native-resha/integration/metro');

module.exports = {
  preset: '@react-native/jest-preset',
  // Resha: resolve `react-native` against the materialized tree (and keep its Flow
  // sources transformable), so tests exercise the overridden JS exactly like Metro
  // does at runtime.
  ...reshaJestConfig(__dirname),
};