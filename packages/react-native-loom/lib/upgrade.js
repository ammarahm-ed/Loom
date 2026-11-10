'use strict';

const {spawnSync} = require('child_process');
const fs = require('fs');
const path = require('path');
const {loadManifest, saveManifest, overridePath, sha256File, PKG_ROOT} = require('./manifest');
const {fetchUpstream} = require('./upstream');

/**
 * Rebase every override from react-native@<current> onto react-native@<to>.
 *
 * For each patch:  git merge-file <ours=src/file> <base=old upstream> <theirs=new upstream>
 *   - upstream unchanged       -> nothing to do
 *   - clean merge              -> src/file updated, baseHash bumped
 *   - conflict                 -> conflict markers written to src/file, reported
 *   - upstream deleted/moved   -> reported for manual handling
 * Platform files are checked for upstream collisions.
 */
function upgrade({to, log = console.log}) {
  const manifest = loadManifest();
  const from = manifest.upstream.version;
  if (from === to) {
    log(`loom: already on react-native@${to}`);
    return {conflicts: [], manual: []};
  }
  log(`loom: fetching react-native@${from} and @${to} from npm...`);
  const baseDir = fetchUpstream(from, manifest.upstream.package);
  const theirsDir = fetchUpstream(to, manifest.upstream.package);

  const report = {clean: [], unchanged: [], conflicts: [], manual: []};
  for (const o of manifest.overrides) {
    const base = path.join(baseDir, o.file);
    const theirs = path.join(theirsDir, o.file);
    if (o.type === 'test') continue; // not in npm; rebase with the fork's git history
    if (o.type === 'platform') {
      if (fs.existsSync(theirs)) report.manual.push(`${o.file}: now exists upstream`);
      continue;
    }
    if (!fs.existsSync(theirs)) {
      report.manual.push(`${o.file}: removed or moved upstream`);
      continue;
    }
    const newHash = sha256File(theirs);
    if (newHash === o.baseHash) {
      report.unchanged.push(o.file);
      continue;
    }
    if (o.type === 'delete') {
      o.baseHash = newHash;
      report.clean.push(o.file);
      continue;
    }
    const ours = overridePath(o);
    const r = spawnSync(
      'git',
      ['merge-file', '-L', 'loom', '-L', `react-native@${from}`, '-L', `react-native@${to}`, ours, base, theirs],
      {encoding: 'utf8'},
    );
    if (r.status < 0 || r.error) throw r.error || new Error(r.stderr);
    o.baseHash = newHash;
    if (r.status === 0) report.clean.push(o.file);
    else report.conflicts.push(`${o.file} (${r.status} conflict${r.status > 1 ? 's' : ''})`);
  }

  manifest.upstream.version = to;
  saveManifest(manifest);
  const pkgPath = path.join(PKG_ROOT, 'package.json');
  const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
  pkg.peerDependencies['react-native'] = to;
  pkg.devDependencies = {...pkg.devDependencies, 'react-native': to};
  fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n');

  log(`loom: ${from} -> ${to}`);
  log(`  unchanged upstream: ${report.unchanged.length}`);
  log(`  merged cleanly:     ${report.clean.length}`);
  log(`  conflicts:          ${report.conflicts.length}`);
  for (const c of report.conflicts) log(`    ! ${c}`);
  log(`  needs manual work:  ${report.manual.length}`);
  for (const m of report.manual) log(`    ? ${m}`);
  if (report.conflicts.length) {
    log('Resolve conflict markers in src/, then run `loom validate`.');
  }
  return report;
}

module.exports = {upgrade};
