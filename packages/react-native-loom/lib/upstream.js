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
  if (fs.existsSync(path.join(pkgDir, 'package.json'))) return pkgDir;
  fs.mkdirSync(dest, {recursive: true});
  const out = execFileSync(
    'npm',
    ['pack', `${packageName}@${version}`, '--silent', '--pack-destination', dest],
    {encoding: 'utf8'},
  ).trim();
  const tgz = path.join(dest, out.split('\n').pop());
  execFileSync('tar', ['-xzf', tgz, '-C', dest]);
  fs.rmSync(tgz);
  return pkgDir;
}

module.exports = {findInstalledReactNative, fetchUpstream, cacheRoot};
