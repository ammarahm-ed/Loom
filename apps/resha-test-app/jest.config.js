const {reshaJestModuleNameMapper} = require('react-native-resha/integration/metro');

module.exports = {
  preset: '@react-native/jest-preset',
  // Resha: resolve `react-native` against the materialized tree so tests exercise
  // the overridden JS, exactly like Metro does at runtime.
  moduleNameMapper: reshaJestModuleNameMapper(__dirname),
};