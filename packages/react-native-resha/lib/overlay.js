'use strict';

const {execFileSync} = require('child_process');
const fs = require('fs');
const path = require('path');
const {loadManifest, overridePath, PKG_ROOT} = require('./manifest');

const REPO_ROOT = path.resolve(PKG_ROOT, '..', '..');
const RN_TESTER_PATCH = path.join(REPO_ROOT, 'examples', 'rn-tester', 'rn-tester.patch');

/**
 * Developer loop: write every override (including Android unit tests, which npm
 * does not ship) into a facebook/react-native checkout at the upstream tag, and
 * apply the rn-tester benchmark patch. Hack there with rn-tester, then bring the
 * work back with `resha import-fork --fork <checkout> --base v<version>`.
 */
function overlay({monorepo, withRnTester = true, log = console.log}) {
  const manifest = loadManifest();
  const rnDir = path.join(monorepo, 'packages', 'react-native');
  if (!fs.existsSync(path.join(rnDir, 'package.json'))) {
    throw new Error(`${monorepo} is not a react-native monorepo checkout`);
  }
  const version = JSON.parse(fs.readFileSync(path.join(rnDir, 'package.json'), 'utf8')).version;
  if (version !== manifest.upstream.version && version !== '1000.0.0') {
    log(`resha: warning: checkout is ${version}, overrides target ${manifest.upstream.version}`);
  }
  let n = 0;
  for (const o of manifest.overrides) {
    const target = path.join(rnDir, o.file);
    if (o.type === 'delete') fs.rmSync(target, {force: true});
    else {
      fs.mkdirSync(path.dirname(target), {recursive: true});
      fs.copyFileSync(overridePath(o), target);
    }
    n++;
  }
  log(`resha: wrote ${n} overrides into ${rnDir}`);
  if (withRnTester && fs.existsSync(RN_TESTER_PATCH)) {
    execFileSync('git', ['-C', monorepo, 'apply', '--3way', RN_TESTER_PATCH], {stdio: 'inherit'});
    log('resha: applied examples/rn-tester/rn-tester.patch');
  }
}

/** Capture rn-tester changes of a monorepo checkout back into the example patch. */
function captureRnTester({fork, base, log = console.log}) {
  const patch = execFileSync(
    'git',
    // --full-index keeps the blob hashes at 40 chars: git abbreviates them by
    // repository object count, so the patch would otherwise differ depending on
    // how large the checkout is.
    ['-C', fork, 'diff', '--binary', '--full-index', base, '--', 'packages/rn-tester', ':!packages/rn-tester/Podfile.lock'],
    {encoding: 'utf8', maxBuffer: 1 << 28},
  );
  // Include untracked new files without touching the fork's index.
  const untracked = execFileSync(
    'git',
    ['-C', fork, 'ls-files', '--others', '--exclude-standard', '--', 'packages/rn-tester'],
    {encoding: 'utf8'},
  ).split('\n').filter(Boolean);
  let extra = '';
  for (const f of untracked) {
    const r = require('child_process').spawnSync(
      'git',
      ['-C', fork, 'diff', '--binary', '--full-index', '--no-index', '--', '/dev/null', f],
      {encoding: 'utf8'},
    );
    extra += r.stdout.replace(/^(diff --git a\/)dev\/null b\//m, `$1${f} b/`);
  }
  if (!patch && !extra) return;
  fs.mkdirSync(path.dirname(RN_TESTER_PATCH), {recursive: true});
  fs.writeFileSync(RN_TESTER_PATCH, patch + extra);
  log(`resha: captured rn-tester changes -> ${path.relative(REPO_ROOT, RN_TESTER_PATCH)}`);
}

module.exports = {overlay, captureRnTester, REPO_ROOT};
