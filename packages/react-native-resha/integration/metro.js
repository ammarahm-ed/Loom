'use strict';

const path = require('path');

/**
 * Point every `react-native` / `react-native/...` import at the materialized
 * tree so JS overrides (e.g. LayoutAnimation.js) are what gets bundled.
 *
 *   // metro.config.js
 *   const {withResha} = require('react-native-resha/integration/metro');
 *   module.exports = withResha(mergeConfig(getDefaultConfig(__dirname), {...}), __dirname);
 */
function withResha(config, projectRoot) {
  const {materialize, materializedDir} = require('../lib/materialize');
  if (process.env.RESHA_SKIP_MATERIALIZE !== '1') {
    materialize({projectRoot, log: () => {}});
  }
  const rnDir = materializedDir(projectRoot);
  const upstream = config.resolver && config.resolver.resolveRequest;

  return {
    ...config,
    resolver: {
      ...config.resolver,
      resolveRequest(context, moduleName, platform) {
        const resolve = upstream || context.resolveRequest;
        if (moduleName === 'react-native' || moduleName.startsWith('react-native/')) {
          const rest = moduleName.slice('react-native'.length);
          return resolve(context, rnDir + rest, platform);
        }
        return resolve(context, moduleName, platform);
      },
    },
    watchFolders: [...(config.watchFolders || []), rnDir],
  };
}

/** Jest: `moduleNameMapper: reshaJestModuleNameMapper(__dirname)` */
function reshaJestModuleNameMapper(projectRoot) {
  const rnDir = path.join(projectRoot, 'node_modules', '.resha', 'react-native');
  return {
    '^react-native$': rnDir,
    '^react-native/(.*)$': `${rnDir}/$1`,
  };
}

module.exports = {withResha, reshaJestModuleNameMapper};
