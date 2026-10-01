'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const PKG_ROOT = path.resolve(__dirname, '..');
const SRC_DIR = path.join(PKG_ROOT, 'src');
const ANDROID_TESTS_DIR = path.join(PKG_ROOT, 'android-tests');
const MANIFEST_PATH = path.join(PKG_ROOT, 'overrides.json');

/**
 * Override entry types (modelled on react-native-windows' overrides.json):
 *
 *  - "patch"    : a full copy of an upstream file with Resha changes applied.
 *                 `baseHash` is the sha256 of the pristine upstream file the
 *                 copy was derived from. If upstream's file no longer matches,
 *                 the override is stale and must go through `resha upgrade`.
 *  - "platform" : a file that does not exist upstream (Resha-only code).
 *  - "delete"   : an upstream file Resha removes.
 *  - "test"     : a test file that upstream does not publish to npm
 *                 (ReactAndroid/src/test/**). Kept for CI, never materialized.
 */
const TYPES = ['patch', 'platform', 'delete', 'test'];

function sha256File(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function loadManifest() {
  const m = JSON.parse(fs.readFileSync(MANIFEST_PATH, 'utf8'));
  for (const o of m.overrides) {
    if (!TYPES.includes(o.type)) {
      throw new Error(`overrides.json: unknown type "${o.type}" for ${o.file}`);
    }
  }
  return m;
}

function saveManifest(m) {
  m.overrides.sort((a, b) => a.file.localeCompare(b.file));
  fs.writeFileSync(MANIFEST_PATH, JSON.stringify(m, null, 2) + '\n');
}

function overridePath(o) {
  return path.join(o.type === 'test' ? ANDROID_TESTS_DIR : SRC_DIR, o.file);
}

module.exports = {
  PKG_ROOT,
  SRC_DIR,
  ANDROID_TESTS_DIR,
  MANIFEST_PATH,
  TYPES,
  sha256File,
  loadManifest,
  saveManifest,
  overridePath,
};
