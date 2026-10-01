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

/** The materialized tree, relative to a `node_modules` directory. */
const MATERIALIZED_RELATIVE = '.resha/react-native';

function loadJestPreset(projectRoot) {
  try {
    return require(require.resolve('@react-native/jest-preset', {paths: [projectRoot]}));
  } catch {
    return {};
  }
}

/**
 * Jest: `moduleNameMapper: reshaJestModuleNameMapper(__dirname)`
 *
 * Jest *replaces* a preset's `moduleNameMapper` with the project's own, so this
 * returns the mappings `@react-native/jest-preset` would have contributed -
 * including aliases such as `react-native/setup-env`, which the generic subpath
 * mapping cannot resolve because it resolves subpaths as literal paths - with
 * every react-native path retargeted at the materialized tree.
 */
function reshaJestModuleNameMapper(projectRoot) {
  const {materializedDir} = require('../lib/materialize');
  const rnDir = materializedDir(projectRoot);
  const mapper = {};

  let upstreamRnDir;
  try {
    upstreamRnDir = path.dirname(require.resolve('react-native', {paths: [projectRoot]}));
  } catch {}

  const presetMapper = loadJestPreset(projectRoot).moduleNameMapper || {};
  for (const [pattern, target] of Object.entries(presetMapper)) {
    mapper[pattern] =
      upstreamRnDir && typeof target === 'string' && target.startsWith(upstreamRnDir)
        ? rnDir + target.slice(upstreamRnDir.length)
        : target;
  }
  // Fallbacks, in case the preset stops mapping the package itself.
  mapper['^react-native$'] = rnDir;
  mapper['^react-native/(.*)$'] = `${rnDir}/$1`;
  return mapper;
}

/**
 * Jest: spread this into the config next to the preset.
 *
 *   const {reshaJestConfig} = require('react-native-resha/integration/metro');
 *   module.exports = {preset: '@react-native/jest-preset', ...reshaJestConfig(__dirname)};
 *
 * Two preset options have to change for a materialized tree:
 *  - `moduleNameMapper`, see above;
 *  - `transformIgnorePatterns`, whose preset value exempts
 *    `node_modules/react-native` from the "ignore node_modules" rule but not
 *    `node_modules/.resha/react-native`, which would leave React Native's Flow
 *    sources untransformed.
 */
function reshaJestConfig(projectRoot) {
  const preset = loadJestPreset(projectRoot);
  const exemption = MATERIALIZED_RELATIVE.replace(/\./g, '\\.');
  const patterns =
    preset.transformIgnorePatterns && preset.transformIgnorePatterns.length
      ? preset.transformIgnorePatterns
      : ['node_modules/(?!((jest-)?react-native|@react-native(-community)?)/)'];

  return {
    moduleNameMapper: reshaJestModuleNameMapper(projectRoot),
    transformIgnorePatterns: patterns.map(pattern =>
      pattern.includes('node_modules/(?!')
        ? pattern.replace('node_modules/(?!', `node_modules/(?!${exemption}|`)
        : pattern,
    ),
  };
}

module.exports = {withResha, reshaJestConfig, reshaJestModuleNameMapper};
