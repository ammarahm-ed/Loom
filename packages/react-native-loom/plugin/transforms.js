'use strict';

// Pure text transforms behind the Expo config plugin, kept free of Expo imports so they can be
// tested against the files `expo prebuild` generates. Every transform is idempotent (a second run
// returns its input) and returns {contents, changed, warning}: `warning` is set when the file no
// longer looks like the template the transform was written for, in which case it is left alone.

const MARK = 'react-native-loom';

function unchanged(contents, warning) {
  return {contents, changed: false, warning};
}

function done(contents) {
  return {contents, changed: true};
}

function isApplied(contents) {
  return contents.includes(MARK);
}

// android/settings.gradle: apply Loom's settings script right after the `plugins {}` block (Gradle
// 9 allows nothing else before it) and before autolinking, which needs the materialized tree.
function settingsGradle(contents) {
  if (isApplied(contents)) {
    return unchanged(contents);
  }
  const plugins = /^plugins\s*\{[^}]*\}[ \t]*\r?\n/m.exec(contents);
  if (!plugins) {
    return unchanged(contents, 'android/settings.gradle has no top-level plugins {} block');
  }
  const block = [
    '',
    `// ${MARK}: materialize the renderer tree and substitute react-android with it`,
    'apply from: new File(',
    '  new File(',
    '    providers.exec {',
    '      workingDir(rootDir)',
    `      commandLine("node", "--print", "require.resolve('${MARK}/package.json')")`,
    '    }.standardOutput.asText.get().trim()',
    '  ).getParentFile(),',
    '  "integration/android/loom.settings.gradle"',
    ')',
    '',
  ].join('\n');
  const end = plugins.index + plugins[0].length;
  return done(contents.slice(0, end) + block + contents.slice(end));
}

// android/app/build.gradle: point the React Native Gradle plugin (codegen, bundling) at the
// materialized tree instead of node_modules/react-native.
function appBuildGradle(contents) {
  if (isApplied(contents)) {
    return unchanged(contents);
  }
  const line = /^([ \t]*)reactNativeDir\s*=.*$/m.exec(contents);
  if (!line) {
    return unchanged(contents, 'android/app/build.gradle has no reactNativeDir assignment');
  }
  const replacement = `${line[1]}reactNativeDir = new File(projectRoot, "node_modules/.loom/react-native") // ${MARK}`;
  return done(contents.replace(line[0], () => replacement));
}

// ios/Podfile: take React Native's pod scripts and path from the materialized tree.
function podfile(contents) {
  if (isApplied(contents)) {
    return unchanged(contents);
  }
  const scripts = /^require File\.join\(File\.dirname\(`node --print "require\.resolve\('react-native\/package\.json'\)"`\), "scripts\/react_native_pods"\)[ \t]*$/m.exec(
    contents,
  );
  if (!scripts) {
    return unchanged(contents, 'ios/Podfile does not require react-native/scripts/react_native_pods the way the Expo template does');
  }
  const header = [
    `# ${MARK}: Loom's React Native tree (materialized on pod install)`,
    `require File.join(File.dirname(\`node --print "require.resolve('${MARK}/package.json')"\`), 'integration/ios/loom_pods')`,
    `require File.join(Loom.react_native_path(__dir__), 'scripts/react_native_pods')`,
  ].join('\n');
  let out = contents.replace(scripts[0], () => header);
  const before = out;
  out = out.replace(/:path => config\[:reactNativePath\]/, () => ':path => Loom.react_native_path(__dir__)');
  out = out.replace(/^(\s*)config\[:reactNativePath\],$/m, (_, indent) => `${indent}Loom.react_native_path(__dir__),`);
  if (out === before) {
    return unchanged(contents, 'ios/Podfile passes no config[:reactNativePath] to use_react_native!');
  }
  return done(out);
}

// metro.config.js: resolve react-native to the materialized tree.
function metroConfig(contents) {
  if (isApplied(contents)) {
    return unchanged(contents);
  }
  const exported = /^module\.exports\s*=\s*([A-Za-z_$][\w$]*)\s*;?[ \t]*$/m.exec(contents);
  if (!exported) {
    return unchanged(
      contents,
      `metro.config.js does not end in "module.exports = config;": wrap it with withLoom from ${MARK}/integration/metro yourself`,
    );
  }
  const name = exported[1];
  const replacement = [
    `// ${MARK}`,
    `const {withLoom} = require('${MARK}/integration/metro');`,
    '',
    `module.exports = withLoom(${name}, __dirname);`,
  ].join('\n');
  return done(contents.replace(exported[0], () => replacement));
}

module.exports = {settingsGradle, appBuildGradle, podfile, metroConfig, MARK};
