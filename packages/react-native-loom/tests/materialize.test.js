'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {test} = require('node:test');
const {materialize, STAMP} = require('../lib/materialize');

// Uses the test app's installed pristine React Native as the validation base.
// All materialization and generated-cache fixtures live in a private temp dir.
test('source refreshes and --force preserve Android build artifacts', () => {
  const projectRoot = path.resolve(__dirname, '../../../apps/loom-test-app');
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'loom-materialize-'));
  const out = path.join(temporary, 'react-native');
  const options = {projectRoot, out, log: () => {}};
  try {
    materialize(options);
    const caches = [
      '.gradle/cache-marker',
      'build/cache-marker',
      'ReactAndroid/build/downloads/boost.tar.gz',
      'ReactAndroid/.cxx/cache-marker',
      'ReactAndroid/hermes-engine/build/cache-marker',
      'ReactAndroid/hermes-engine/.cxx/cache-marker',
    ];
    const inodes = new Map();
    for (const relative of caches) {
      const file = path.join(out, relative);
      fs.mkdirSync(path.dirname(file), {recursive: true});
      fs.writeFileSync(file, relative);
      inodes.set(relative, fs.statSync(file).ino);
    }
    for (const force of [false, true]) {
      const staleSource = path.join(out, 'ReactCommon/stale-source.txt');
      fs.writeFileSync(staleSource, 'must disappear');
      const stampPath = path.join(out, STAMP);
      const stamp = JSON.parse(fs.readFileSync(stampPath, 'utf8'));
      stamp.hash = 'previous override revision';
      fs.writeFileSync(stampPath, JSON.stringify(stamp));
      materialize({...options, force});
      assert.equal(fs.existsSync(staleSource), false);
      for (const relative of caches) {
        const file = path.join(out, relative);
        assert.equal(fs.readFileSync(file, 'utf8'), relative);
        assert.equal(fs.statSync(file).ino, inodes.get(relative));
      }
    }
  } finally {
    fs.rmSync(temporary, {recursive: true, force: true});
  }
});
