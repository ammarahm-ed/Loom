'use strict';

const fs = require('fs');
const path = require('path');
const {loadManifest, overridePath, sha256File} = require('./manifest');

/** `0.<minor>` of a version string ("0.89.2-rc.1" -> "0.89"). */
function minorOf(version) {
  const m = /^(\d+)\.(\d+)\./.exec(version || '');
  return m ? `${m[1]}.${m[2]}` : null;
}

/** Every pristine upstream hash an override is known to apply to (its base, plus later patch releases). */
function knownBaseHashes(o) {
  return new Set([o.baseHash, ...Object.values(o.baseHashes || {})].filter(Boolean));
}

/**
 * Validates the installed react-native against Loom's overrides.
 *
 * Returns {errors, warnings}. `strict` (Loom's own CI, `loom validate`) treats every finding as an
 * error. In an app (`loom materialize`):
 *  - The installed react-native must be the same minor as Loom's (each Loom release supports one
 *    React Native minor). Any other minor is an error.
 *  - A patch release Loom has not verified is accepted with a warning when every file Loom replaces
 *    is still one Loom knows; it is an error only when React Native changed a file Loom replaces.
 *  - On a verified release, an overridden file that differs from upstream was patched by the app
 *    (patch-package, pnpm patch): Loom's copy replaces it, which is reported as a warning.
 *
 * Checks (same spirit as react-native-windows' `validate-overrides`): every patch's upstream file
 * hashes to a known base, every platform file does not (yet) exist upstream, every override file
 * exists on disk, and (strict only) no stray files exist in src/.
 */
function validateDetailed({upstreamDir, upstreamVersion, manifest = loadManifest(), strict = false}) {
  const errors = [];
  const warnings = [];
  const supported = manifest.upstream.supported || [manifest.upstream.version];
  const loomMinor = manifest.upstream.minor || minorOf(manifest.upstream.version);
  const verified = !upstreamVersion || supported.includes(upstreamVersion);
  if (upstreamVersion && minorOf(upstreamVersion) !== loomMinor) {
    errors.push(
      `installed react-native@${upstreamVersion}, but this Loom supports React Native ${loomMinor}.x only ` +
        `(install the Loom release for React Native ${minorOf(upstreamVersion)}, or react-native@${loomMinor}.x)`,
    );
    return {errors, warnings};
  }
  if (!verified) {
    warnings.push(
      `react-native@${upstreamVersion} is not yet verified by this Loom (verified: ${supported.join(', ')}); ` +
        'continuing because none of the files Loom replaces changed',
    );
  }
  const known = new Set();
  for (const o of manifest.overrides) {
    const own = overridePath(o);
    known.add(own);
    if (o.type !== 'delete' && !fs.existsSync(own)) {
      errors.push(`${o.file}: override file missing`);
    }
    const up = path.join(upstreamDir, o.file);
    if (o.type === 'patch' || o.type === 'delete') {
      if (!fs.existsSync(up) && o.monorepoOnly) {
        // Build tooling that exists only in the react-native monorepo (Loom's own prebuild), not in the npm package.
        continue;
      }
      if (!fs.existsSync(up)) {
        errors.push(`${o.file}: ${o.type} target no longer exists upstream`);
      } else if (!knownBaseHashes(o).has(sha256File(up))) {
        if (strict) {
          errors.push(`${o.file}: upstream changed since override was based (baseHash mismatch)`);
        } else if (verified) {
          warnings.push(
            `${o.file}: differs from react-native@${upstreamVersion} (patched by the app?); Loom's copy replaces it, ` +
              'so a patch to this file has no effect',
          );
        } else {
          errors.push(
            `${o.file}: react-native@${upstreamVersion} changed this file, which Loom replaces; ` +
              `update react-native-loom to a release that supports ${upstreamVersion}`,
          );
        }
      }
    } else if (o.type === 'platform' && fs.existsSync(up)) {
      errors.push(`${o.file}: platform file now exists upstream; convert to patch or drop`);
    }
  }
  for (const root of strict ? ['src', 'android-tests'] : []) {
    const dir = path.join(__dirname, '..', root);
    for (const f of walk(dir)) {
      if (!known.has(f)) errors.push(`${path.relative(path.join(__dirname, '..'), f)}: not listed in overrides.json`);
    }
  }
  return {errors, warnings};
}

/** Strict validation (Loom's CI, `loom validate`): every finding is an error. */
function validate(options) {
  const {errors, warnings} = validateDetailed({...options, strict: true});
  return [...errors, ...warnings];
}

function* walk(dir) {
  if (!fs.existsSync(dir)) return;
  for (const e of fs.readdirSync(dir, {withFileTypes: true})) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    else if (e.name !== '.gitkeep') yield p;
  }
}

module.exports = {validate, validateDetailed, minorOf, walk};
