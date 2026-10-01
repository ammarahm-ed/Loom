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
  // Absolute: the rewritten module names go straight back into Metro, which
  // only treats them as paths (rather than package names) when they are rooted.
  const rnDir = path.resolve(materializedDir(projectRoot));
  const upstream = config.resolver && config.resolver.resolveRequest;
  const serializer = config.serializer || {};
  const upstreamRunBeforeMainModule = serializer.getModulesRunBeforeMainModule;

  return {
    ...config,
    resolver: {
      ...config.resolver,
      resolveRequest(context, moduleName, platform) {
        const resolve = upstream || context.resolveRequest;
        if (moduleName === 'react-native' || moduleName.startsWith('react-native/')) {
          const rest = moduleName.slice('react-native'.length);
          return resolve(
            context,
            rest ? rnDir + exportsSubpath(rnDir, rest) : rnDir,
            platform,
          );
        }
        return resolve(context, moduleName, platform);
      },
    },
    serializer: {
      ...serializer,
      getModulesRunBeforeMainModule: entryPoint =>
        (upstreamRunBeforeMainModule
          ? upstreamRunBeforeMainModule.call(serializer, entryPoint)
          : []
        ).map(modulePath => retargetReactNativePath(modulePath, rnDir)),
    },
    watchFolders: [...(config.watchFolders || []), rnDir],
  };
}

/**
 * Metro only emits the "run before main module" statements for paths that are
 * present verbatim in the bundle graph — metro/src/lib/getAppendScripts.js
 * compares `module.path === path` and silently drops every entry that does not
 * match. The default config resolves `react-native/setup-env` with Node, which
 * finds the pristine package, while every module in the graph now comes from
 * `rnDir`. Retarget the path, or React Native's JavaScript environment is never
 * set up: `window`, the timers, the console and the HMR client all go missing.
 */
function retargetReactNativePath(modulePath, rnDir) {
  const marker = `${path.sep}node_modules${path.sep}react-native${path.sep}`;
  const index = modulePath.lastIndexOf(marker);
  if (index === -1) {
    return modulePath;
  }
  return path.join(rnDir, modulePath.slice(index + marker.length));
}

/**
 * A `react-native/...` subpath has to be looked up in the package's own
 * `exports` map before falling back to a literal file path: entries such as
 * `./asset-registry` and `./setup-env` have no file at the package root, so
 * they only resolve through the map. Pattern entries (`./Libraries/*`) are not
 * literal keys and are real files, which is what the fallback covers.
 *
 * The returned value keeps its leading slash, so it can be concatenated onto
 * the package directory.
 */
function exportsSubpath(packageDir, rest) {
  let exportsMap;
  try {
    exportsMap = JSON.parse(
      require('fs').readFileSync(path.join(packageDir, 'package.json'), 'utf8'),
    ).exports;
  } catch {
    return rest;
  }
  const target = pickCondition(exportsMap && exportsMap['.' + rest]);
  return target && target.startsWith('./') ? target.slice(1) : rest;
}

/** The first usable target of an `exports` entry (string / array / conditions). */
function pickCondition(value) {
  if (typeof value === 'string') {
    return value;
  }
  if (Array.isArray(value)) {
    for (const entry of value) {
      const picked = pickCondition(entry);
      if (picked) {
        return picked;
      }
    }
    return null;
  }
  if (value && typeof value === 'object') {
    if (typeof value.default === 'string') {
      return value.default;
    }
    for (const entry of Object.values(value)) {
      const picked = pickCondition(entry);
      if (picked) {
        return picked;
      }
    }
  }
  return null;
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
