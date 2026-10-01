'use strict';

const fs = require('fs');
const path = require('path');
const {loadManifest, overridePath, sha256File} = require('./manifest');

/**
 * Returns a list of human readable errors. Empty list == valid.
 *
 * Checks (same spirit as react-native-windows' `validate-overrides`):
 *  - installed react-native version equals the version overrides were based on
 *  - every patch's upstream file still hashes to its recorded baseHash
 *  - every platform file does not (yet) exist upstream
 *  - every override file exists on disk, and no stray files exist in src/
 */
function validate({upstreamDir, upstreamVersion, manifest = loadManifest()}) {
  const errors = [];
  if (upstreamVersion && upstreamVersion !== manifest.upstream.version) {
    errors.push(
      `installed react-native@${upstreamVersion} but overrides target ${manifest.upstream.version} (run \`resha upgrade --to ${upstreamVersion}\`)`,
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
      if (!fs.existsSync(up)) {
        errors.push(`${o.file}: ${o.type} target no longer exists upstream`);
      } else if (sha256File(up) !== o.baseHash) {
        errors.push(`${o.file}: upstream changed since override was based (baseHash mismatch)`);
      }
    } else if (o.type === 'platform' && fs.existsSync(up)) {
      errors.push(`${o.file}: platform file now exists upstream; convert to patch or drop`);
    }
  }
  for (const root of ['src', 'android-tests']) {
    const dir = path.join(__dirname, '..', root);
    for (const f of walk(dir)) {
      if (!known.has(f)) errors.push(`${path.relative(path.join(__dirname, '..'), f)}: not listed in overrides.json`);
    }
  }
  return errors;
}

function* walk(dir) {
  if (!fs.existsSync(dir)) return;
  for (const e of fs.readdirSync(dir, {withFileTypes: true})) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) yield* walk(p);
    else if (e.name !== '.gitkeep') yield p;
  }
}

module.exports = {validate, walk};
