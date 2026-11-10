'use strict';

const {execFileSync} = require('child_process');
const fs = require('fs');
const path = require('path');
const {loadManifest, saveManifest, sha256File, SRC_DIR, ANDROID_TESTS_DIR, MANIFEST_PATH} = require('./manifest');
const {classify} = require('./areas');
const {fetchUpstream} = require('./upstream');
const {captureRnTester} = require('./overlay');

const PREFIX = 'packages/react-native/';
const IGNORE = [/\.bak$/, /(^|\/)\.r\d+_/, /\.DS_Store$/];

/**
 * Migration helper: turn the working tree of an in-tree RN fork into overrides.
 * Idempotent; re-run it while fork development continues until the fork is retired.
 *
 *   loom import-fork --fork ~/Work/loom --base a59eff64fa --version 0.87.1
 */
function importFork({fork, base, version, log = console.log}) {
  const git = (...args) => execFileSync('git', ['-C', fork, ...args], {encoding: 'utf8', maxBuffer: 1 << 28});
  const upstreamDir = fetchUpstream(version);

  const changes = new Map();
  for (const line of git('diff', '--name-status', '--no-renames', base, '--', PREFIX).split('\n')) {
    if (!line) continue;
    const [status, file] = line.split('\t');
    changes.set(file, status[0]);
  }
  for (const file of git('ls-files', '--others', '--exclude-standard', '--', PREFIX).split('\n')) {
    if (file) changes.set(file, 'A');
  }

  const manifest = fs.existsSync(MANIFEST_PATH)
    ? loadManifest()
    : {upstream: {package: 'react-native', version}, overrides: []};
  manifest.upstream.version = version;
  const prevAreas = new Map(manifest.overrides.map(o => [o.file, o.area]));
  manifest.overrides = [];
  fs.rmSync(SRC_DIR, {recursive: true, force: true});
  fs.rmSync(ANDROID_TESTS_DIR, {recursive: true, force: true});

  const skipped = [];
  for (const [repoFile, status] of [...changes].sort()) {
    if (IGNORE.some(re => re.test(repoFile))) {
      skipped.push(repoFile);
      continue;
    }
    const file = repoFile.slice(PREFIX.length);
    const up = path.join(upstreamDir, file);
    const area = prevAreas.get(file) || classify(file);
    const isAndroidTest = file.startsWith('ReactAndroid/src/test/');

    if (status === 'D') {
      if (fs.existsSync(up)) manifest.overrides.push({type: 'delete', file, baseHash: sha256File(up), area});
      continue;
    }
    let type;
    if (isAndroidTest) type = 'test';
    else if (fs.existsSync(up)) type = 'patch';
    else type = 'platform';

    const dest = path.join(type === 'test' ? ANDROID_TESTS_DIR : SRC_DIR, file);
    fs.mkdirSync(path.dirname(dest), {recursive: true});
    fs.copyFileSync(path.join(fork, repoFile), dest);
    const entry = {type, file, area};
    if (type === 'patch') {
      if (sha256File(up) === sha256File(dest)) {
        fs.rmSync(dest); // identical to upstream; not an override
        continue;
      }
      entry.baseHash = sha256File(up);
    }
    manifest.overrides.push(entry);
  }
  saveManifest(manifest);
  const count = t => manifest.overrides.filter(o => o.type === t).length;
  log(
    `loom: imported ${manifest.overrides.length} overrides from ${fork} ` +
      `(patch ${count('patch')}, platform ${count('platform')}, delete ${count('delete')}, test ${count('test')})`,
  );
  captureRnTester({fork, base, log});
  if (skipped.length) log(`  skipped ${skipped.length} scratch files (${skipped.join(', ')})`);
}

module.exports = {importFork};
