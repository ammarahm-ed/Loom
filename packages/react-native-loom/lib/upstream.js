'use strict';

const {execFileSync} = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

/** Locate the pristine react-native package installed in the app. */
function findInstalledReactNative(projectRoot) {
  const pkgJson = require.resolve('react-native/package.json', {
    paths: [projectRoot],
  });
  const dir = path.dirname(fs.realpathSync(pkgJson));
  const version = JSON.parse(fs.readFileSync(pkgJson, 'utf8')).version;
  return {dir, version};
}

function cacheRoot() {
  return (
    process.env.LOOM_CACHE_DIR ||
    path.join(os.homedir(), '.cache', 'react-native-loom')
  );
}

/**
 * Fetch and extract a pristine react-native@version from the npm registry.
 * This is what the 3-way merge in `loom upgrade` uses for the base and for
 * "theirs", so it never depends on what happens to be installed.
 */
function fetchUpstream(version, packageName = 'react-native') {
  const dest = path.join(cacheRoot(), `${packageName}-${version}`);
  const pkgDir = path.join(dest, 'package');
  // Only a completed extraction is ever at `dest` (see the rename below), so this is never a
  // half-extracted tree left by an interrupted run.
  if (fs.existsSync(path.join(pkgDir, 'package.json'))) return pkgDir;
  fs.mkdirSync(cacheRoot(), {recursive: true});
  const tmp = fs.mkdtempSync(path.join(cacheRoot(), `.${packageName}-${version}-`));
  try {
    // `npm pack` checks the tarball against the registry's integrity hash.
    const out = execFileSync(
      'npm',
      ['pack', `${packageName}@${version}`, '--silent', '--pack-destination', tmp],
      {encoding: 'utf8'},
    ).trim();
    const tgz = path.join(tmp, out.split('\n').pop());
    execFileSync('tar', ['-xzf', tgz, '-C', tmp]);
    fs.rmSync(tgz);
    if (!fs.existsSync(path.join(tmp, 'package', 'package.json'))) {
      throw new Error(`${packageName}@${version}: the tarball has no package/package.json`);
    }
    fs.rmSync(dest, {recursive: true, force: true});
    try {
      fs.renameSync(tmp, dest);
    } catch (e) {
      // Another process finished the same version first; theirs is complete too.
      if (!fs.existsSync(path.join(pkgDir, 'package.json'))) throw e;
    }
  } finally {
    fs.rmSync(tmp, {recursive: true, force: true});
  }
  return pkgDir;
}

module.exports = {findInstalledReactNative, fetchUpstream, cacheRoot};
