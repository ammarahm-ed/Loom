const fs = require('fs');
const {getDefaultConfig, mergeConfig} = require('@react-native/metro-config');
const {withLoom} = require('react-native-loom/integration/metro');

// The compat screens post their results to /compat-report on this dev server (the same origin as
// the bundle, so it is reachable from simulators, emulators and phones alike). Each result is
// appended as one JSON line to $COMPAT_RESULTS and printed.
const results = process.env.COMPAT_RESULTS;
const compatReport = {
  server: {
    enhanceMiddleware: middleware => (req, res, next) => {
      if (!req.url.startsWith('/compat-report')) {
        return middleware(req, res, next);
      }
      let body = '';
      req.on('data', c => (body += c));
      req.on('end', () => {
        if (body && results) {
          fs.appendFileSync(results, body.replace(/\n/g, ' ') + '\n');
        }
        res.end('ok');
      });
    },
  },
};

// LOOM_BENCH_STOCK=1 bundles stock react-native (the `stock` variant of scripts/build.sh).
const merged = mergeConfig(getDefaultConfig(__dirname), compatReport);
module.exports =
  process.env.LOOM_BENCH_STOCK === '1' ? merged : withLoom(merged, __dirname);
