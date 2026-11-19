'use strict';

const fs = require('node:fs');
const path = require('node:path');
const transforms = require('./transforms');

function loadConfigPlugins() {
  try {
    return require('expo/config-plugins');
  } catch {
    return require('@expo/config-plugins');
  }
}

const {withSettingsGradle, withAppBuildGradle, withGradleProperties, withPodfile, withPodfileProperties, withDangerousMod} =
  loadConfigPlugins();

const DEFAULTS = {renderer: true, js: 'background', compileFromSource: false, releaseProfiling: false};

function resolveOptions(props) {
  const options = {...DEFAULTS, ...(props || {})};
  for (const key of ['renderer', 'compileFromSource', 'releaseProfiling']) {
    if (typeof options[key] !== 'boolean') {
      throw new Error(`react-native-loom: "${key}" must be true or false, got ${JSON.stringify(options[key])}`);
    }
  }
  if (options.js !== 'main' && options.js !== 'background') {
    throw new Error(`react-native-loom: "js" must be "main" or "background", got ${JSON.stringify(options.js)}`);
  }
  return options;
}

// Same names as android/gradle.properties and ios/Podfile.properties.json.
function switches(options) {
  return {
    'loom.renderer': String(options.renderer),
    'loom.js': options.js,
    'loom.compileFromSource': String(options.compileFromSource),
    'loom.releaseProfiling': String(options.releaseProfiling),
  };
}

function apply(transform, contents, file) {
  const result = transform(contents);
  if (result.warning) {
    console.warn(`react-native-loom: ${file}: ${result.warning}. Left unchanged; see the getting-started guide.`);
  }
  return result.contents;
}

function withLoomAndroid(config, options) {
  config = withSettingsGradle(config, c => {
    c.modResults.contents = apply(transforms.settingsGradle, c.modResults.contents, 'android/settings.gradle');
    return c;
  });
  config = withAppBuildGradle(config, c => {
    c.modResults.contents = apply(transforms.appBuildGradle, c.modResults.contents, 'android/app/build.gradle');
    return c;
  });
  return withGradleProperties(config, c => {
    const values = switches(options);
    c.modResults = c.modResults.filter(item => !(item.type === 'property' && item.key in values));
    for (const [key, value] of Object.entries(values)) {
      c.modResults.push({type: 'property', key, value});
    }
    return c;
  });
}

function withLoomIos(config, options) {
  config = withPodfile(config, c => {
    c.modResults.contents = apply(transforms.podfile, c.modResults.contents, 'ios/Podfile');
    return c;
  });
  return withPodfileProperties(config, c => {
    Object.assign(c.modResults, switches(options));
    return c;
  });
}

// metro.config.js lives in the project, not in a platform folder; patch it once per prebuild.
function withLoomMetro(config) {
  return withDangerousMod(config, [
    'ios',
    async c => {
      const file = path.join(c.modRequest.projectRoot, 'metro.config.js');
      if (!fs.existsSync(file)) {
        console.warn(
          'react-native-loom: no metro.config.js. Create one with `npx expo customize metro.config.js` and wrap it with withLoom from react-native-loom/integration/metro.',
        );
        return c;
      }
      const before = fs.readFileSync(file, 'utf8');
      const after = apply(transforms.metroConfig, before, 'metro.config.js');
      if (after !== before) {
        fs.writeFileSync(file, after);
      }
      return c;
    },
  ]);
}

/**
 * Options (all optional; same meaning as the `loom.*` switches in getting-started):
 *   renderer           boolean  Loom renderer on (default true); false keeps stock React Native
 *   js                 'main' | 'background'  where JavaScript runs (default 'background')
 *   compileFromSource  boolean  build Loom from source instead of the prebuilt (default false)
 *   releaseProfiling   boolean  Release builds keep DevTools and tracing (default false)
 *
 *   ["react-native-loom", {"js": "main"}]
 */
function withLoom(config, props) {
  const options = resolveOptions(props);
  config = withLoomAndroid(config, options);
  config = withLoomIos(config, options);
  return withLoomMetro(config);
}

module.exports = withLoom;
module.exports.resolveOptions = resolveOptions;
module.exports.switches = switches;
