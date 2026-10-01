'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const {loadManifest, overridePath, sha256File, PKG_ROOT} = require('./manifest');
const {findInstalledReactNative} = require('./upstream');
const {validate} = require('./validate');

const STAMP = '.resha-stamp.json';

/**
 * Compile-time switches read by the patched ReactAndroid/build.gradle.kts.
 * An included Gradle build does not see the app's gradle.properties, so we copy
 * these from <app>/android/gradle.properties into the materialized root
 * gradle.properties. `-P` on the command line still wins (Gradle start parameters
 * propagate to included builds).
 */
const GRADLE_SWITCHES = [
  'RN_UI_THREAD',
  'UI_THREAD_RENDERER',
  'UI_THREAD_RENDERER_WORKER',
  'RN_PROFILE_UI_THREAD_RENDERER',
  'RN_BUILD_CPP_TESTS',
];

function readGradleSwitches(projectRoot) {
  const file = path.join(projectRoot, 'android', 'gradle.properties');
  const out = {};
  if (!fs.existsSync(file)) return out;
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    const m = /^\s*([A-Z_]+)\s*=\s*(\S+)\s*$/.exec(line);
    if (m && GRADLE_SWITCHES.includes(m[1])) out[m[1]] = m[2];
  }
  return out;
}

function materializedDir(projectRoot) {
  return path.join(projectRoot, 'node_modules', '.resha', 'react-native');
}

function computeStamp(manifest, upstream, switches) {
  const h = crypto.createHash('sha256');
  h.update(upstream.version);
  h.update(JSON.stringify(switches));
  h.update(JSON.stringify(manifest));
  for (const o of manifest.overrides) {
    if (o.type === 'patch' || o.type === 'platform') {
      h.update(o.file);
      h.update(sha256File(overridePath(o)));
    }
  }
  const pkg = JSON.parse(fs.readFileSync(path.join(PKG_ROOT, 'package.json'), 'utf8'));
  return {reshaVersion: pkg.version, upstreamVersion: upstream.version, gradleSwitches: switches, hash: h.digest('hex')};
}

/**
 * Build node_modules/.resha/react-native = pristine react-native + overrides.
 *
 * node_modules/react-native is never modified: it stays the 3-way merge base
 * and the reference `validate` hashes against. Every consumer (Gradle, CocoaPods,
 * Metro, codegen) is pointed at the materialized copy by the integration helpers.
 */
function materialize({projectRoot, force = false, out, log = console.log}) {
  const manifest = loadManifest();
  const upstream = findInstalledReactNative(projectRoot);
  const errors = validate({upstreamDir: upstream.dir, upstreamVersion: upstream.version, manifest});
  if (errors.length) {
    throw new Error('Refusing to materialize; overrides are invalid:\n  ' + errors.join('\n  '));
  }

  const dest = out ? path.resolve(out) : materializedDir(projectRoot);
  const switches = readGradleSwitches(projectRoot);
  const stamp = computeStamp(manifest, upstream, switches);
  const stampFile = path.join(dest, STAMP);
  if (!force && fs.existsSync(stampFile)) {
    const prev = JSON.parse(fs.readFileSync(stampFile, 'utf8'));
    if (prev.hash === stamp.hash) {
      log(`resha: ${path.relative(projectRoot, dest)} is up to date`);
      return dest;
    }
  }

  const tmp = dest + '.tmp-' + process.pid;
  fs.rmSync(tmp, {recursive: true, force: true});
  fs.mkdirSync(path.dirname(dest), {recursive: true});
  // Copy the pristine package. Keep symlinks inside nested node_modules as-is.
  fs.cpSync(upstream.dir, tmp, {recursive: true, verbatimSymlinks: true});

  let applied = 0;
  for (const o of manifest.overrides) {
    const target = path.join(tmp, o.file);
    if (o.type === 'patch' || o.type === 'platform') {
      fs.mkdirSync(path.dirname(target), {recursive: true});
      fs.copyFileSync(overridePath(o), target);
      applied++;
    } else if (o.type === 'delete') {
      fs.rmSync(target, {force: true});
      applied++;
    }
  }
  if (Object.keys(switches).length) {
    const gp = path.join(tmp, 'gradle.properties');
    const lines = Object.entries(switches).map(([k, v]) => `${k}=${v}`);
    fs.appendFileSync(gp, `\n# Resha switches copied from android/gradle.properties by \`resha materialize\`\n${lines.join('\n')}\n`);
  }
  fs.writeFileSync(path.join(tmp, STAMP), JSON.stringify(stamp, null, 2) + '\n');
  fs.rmSync(dest, {recursive: true, force: true});
  fs.renameSync(tmp, dest);
  log(
    `resha: materialized react-native@${upstream.version} + ${applied} overrides -> ${path.relative(projectRoot, dest)}`,
  );
  return dest;
}

module.exports = {materialize, materializedDir, readGradleSwitches, GRADLE_SWITCHES, STAMP};
