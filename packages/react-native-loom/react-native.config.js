// Autolinking for react-native-loom: links the Reanimated adapter's iOS pod when
// a Reanimated version it supports is installed (Android builds the adapter into
// Reanimated's own library; see adapters/reanimated/README.md).
const fs = require('fs');

function reanimatedAdapterSupported() {
  if (process.env.LOOM_BENCH_STOCK === '1') {
    return false;
  }
  try {
    const pkg = require.resolve('react-native-reanimated/package.json', {paths: [process.cwd()]});
    const {version} = JSON.parse(fs.readFileSync(pkg, 'utf8'));
    const {versions} = require('./adapters/reanimated/supported.json');
    return versions.includes(version);
  } catch {
    return false;
  }
}

module.exports = {
  dependency: {
    platforms: {
      // The CLI finds RNLoomReanimated.podspec in this package's root.
      ios: reanimatedAdapterSupported() ? {} : null,
      android: null,
    },
  },
};
