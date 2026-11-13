module.exports = {
  ...(process.env.LOOM_BENCH_STOCK === '1' ? {} : {reactNativePath: './node_modules/.loom/react-native'}),
};
