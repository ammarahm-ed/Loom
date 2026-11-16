'use strict';

const {spawnSync} = require('child_process');
const fs = require('fs');
const path = require('path');
const {loadManifest, saveManifest, overridePath, sha256File, PKG_ROOT} = require('./manifest');
const {fetchUpstream} = require('./upstream');
const {minorOf} = require('./validate');

/**
 * Rebase every override from react-native@<current> onto react-native@<to>.
 *
 * For each patch:  git merge-file <ours=src/file> <base=old upstream> <theirs=new upstream>
 *   - upstream unchanged       -> nothing to do
 *   - clean merge              -> src/file updated, baseHash bumped
 *   - conflict                 -> conflict markers written to src/file, reported
 *   - upstream deleted/moved   -> reported for manual handling
 * Platform files are checked for upstream collisions.
 *
 * Each Loom release supports one React Native minor and lists the patch releases it was verified on
 * (`upstream.supported`). Within the minor (`--to 0.89.2` from 0.89.1), a release that changed none
 * of the files Loom replaces is simply added to that list; one that did changes the overrides, so
 * the list restarts at that release (the rebased copies are not known to fit the older ones). A new
 * minor is a new Loom line: the list restarts and the peer range moves to that minor.
 */
function upgrade({to, dryRun = false, log = console.log}) {
  const manifest = loadManifest();
  const from = manifest.upstream.version;
  if (from === to || (manifest.upstream.supported || []).includes(to)) {
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
    // --dry-run merges to stdout (`-p`) and writes nothing: CI uses it to see what a release needs.
    const r = spawnSync(
      'git',
      ['merge-file', ...(dryRun ? ['-p'] : []), '-L', 'loom', '-L', `react-native@${from}`, '-L', `react-native@${to}`, ours, base, theirs],
      {encoding: 'utf8', maxBuffer: 64 * 1024 * 1024},
    );
    if (r.status < 0 || r.error) throw r.error || new Error(r.stderr);
    o.baseHash = newHash;
    if (r.status === 0) report.clean.push(o.file);
    else report.conflicts.push(`${o.file} (${r.status} conflict${r.status > 1 ? 's' : ''})`);
  }

  if (dryRun) {
    log(`loom: ${from} -> ${to} (dry run, nothing written)`);
    log(`  unchanged upstream: ${report.unchanged.length}`);
    log(`  would merge cleanly: ${report.clean.length}`);
    log(`  conflicts:          ${report.conflicts.length}`);
    for (const c of report.conflicts) log(`    ! ${c}`);
    log(`  needs manual work:  ${report.manual.length}`);
    for (const m of report.manual) log(`    ? ${m}`);
    return report;
  }
  const sameMinor = minorOf(from) === minorOf(to);
  const changedOverrides = report.clean.length + report.conflicts.length + report.manual.length;
  const previous = manifest.upstream.supported || [from];
  manifest.upstream.minor = minorOf(to);
  manifest.upstream.version = to;
  manifest.upstream.supported =
    sameMinor && changedOverrides === 0 ? [...new Set([...previous, to])] : [to];
  saveManifest(manifest);
  const pkgPath = path.join(PKG_ROOT, 'package.json');
  const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
  const [major, minor] = minorOf(to).split('.').map(Number);
  const lowest = manifest.upstream.supported[0];
  pkg.peerDependencies['react-native'] = `>=${lowest} <${major}.${minor + 1}.0`;
  pkg.devDependencies = {...pkg.devDependencies, 'react-native': to};
  fs.writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n');
  if (!sameMinor) {
    log(`loom: new React Native minor ${minorOf(to)}: bump react-native-loom to ${minorOf(to)}.0 before releasing.`);
  }
  log(`  verified React Native releases: ${manifest.upstream.supported.join(', ')}`);

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
