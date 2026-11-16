'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const {loadManifest, overridePath, sha256File, PKG_ROOT} = require('./manifest');
const {findInstalledReactNative} = require('./upstream');
const {validateDetailed} = require('./validate');

const STAMP = '.loom-stamp.json';
/** Files materialize wrote last time (relative paths), so a refresh removes only its own files. */
const FILES = '.loom-files.json';
const LOCK = '.loom-materialize.lock';

/**
 * Compile-time switches read by the patched ReactAndroid/build.gradle.kts. An included Gradle build
 * does not see the app's gradle.properties, so every `loom.*` key of <app>/android/gradle.properties
 * is copied into the materialized root gradle.properties. `-P` on the command line still wins (Gradle
 * start parameters propagate to included builds). Listed for documentation; any `loom.*` key is copied.
 */
const GRADLE_SWITCHES = [
  'loom.renderer',
  'loom.js',
  'loom.profile',
  'loom.cppTests',
  'loom.leanRelease',
  'loom.releaseProfiling',
];

function readGradleSwitches(projectRoot) {
  const file = path.join(projectRoot, 'android', 'gradle.properties');
  const out = {};
  if (!fs.existsSync(file)) return out;
  for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const m = /^\s*(loom\.[A-Za-z0-9_.-]+)\s*[=:]\s*(.*?)\s*$/.exec(line);
    if (m) out[m[1]] = m[2];
  }
  return out;
}

function materializedDir(projectRoot) {
  return path.join(projectRoot, 'node_modules', '.loom', 'react-native');
}

/**
 * What the tree's content depends on: the React Native version and Loom's overrides. The switches
 * are deliberately not part of it: they only change the small gradle.properties block written on
 * every run, so flipping an Android switch never rewrites the tree under a running iOS build.
 */
function computeStamp(manifest, upstream) {
  const h = crypto.createHash('sha256');
  h.update(upstream.version);
  h.update(upstream.dir);
  h.update(JSON.stringify(manifest));
  for (const o of manifest.overrides) {
    if (o.type === 'patch' || o.type === 'platform') {
      h.update(o.file);
      h.update(sha256File(overridePath(o)));
    }
  }
  const pkg = JSON.parse(fs.readFileSync(path.join(PKG_ROOT, 'package.json'), 'utf8'));
  return {loomVersion: pkg.version, upstreamVersion: upstream.version, hash: h.digest('hex')};
}

// ---- Locking ------------------------------------------------------------------------------------

function sleep(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

function processAlive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return e.code === 'EPERM';
  }
}

/**
 * An exclusive lock next to the tree (Gradle settings evaluation and `pod install` both
 * materialize, and can run at once: CI building both platforms, an IDE sync during a terminal
 * build, worktrees sharing a root). A lock whose owner died is taken over.
 */
function withLock(dir, log, body) {
  fs.mkdirSync(dir, {recursive: true});
  const lockFile = path.join(dir, LOCK);
  const started = Date.now();
  let warned = false;
  for (;;) {
    try {
      const fd = fs.openSync(lockFile, 'wx');
      fs.writeSync(fd, String(process.pid));
      fs.closeSync(fd);
      break;
    } catch (e) {
      if (e.code !== 'EEXIST') throw e;
      let owner = NaN;
      try {
        owner = parseInt(fs.readFileSync(lockFile, 'utf8'), 10);
      } catch {}
      if (!Number.isNaN(owner) && !processAlive(owner)) {
        fs.rmSync(lockFile, {force: true});
        continue;
      }
      if (Date.now() - started > 10 * 60 * 1000) {
        throw new Error(`loom: ${lockFile} is held by process ${owner} for over 10 minutes; remove it if that process is gone`);
      }
      if (!warned) {
        log(`loom: waiting for another materialize (process ${owner}) to finish...`);
        warned = true;
      }
      sleep(250);
    }
  }
  try {
    return body();
  } finally {
    fs.rmSync(lockFile, {force: true});
  }
}

// ---- Tree sync ------------------------------------------------------------------------------------

/** Relative paths of every file under `root` (symlinks included as entries, not followed). */
function listFiles(root) {
  const out = [];
  const stack = [''];
  while (stack.length) {
    const rel = stack.pop();
    for (const e of fs.readdirSync(path.join(root, rel), {withFileTypes: true})) {
      const r = rel ? path.join(rel, e.name) : e.name;
      if (e.isDirectory()) stack.push(r);
      else out.push(r);
    }
  }
  return out;
}

function sameFile(a, b) {
  let sa, sb;
  try {
    sa = fs.lstatSync(a);
    sb = fs.lstatSync(b);
  } catch {
    return false;
  }
  if (sa.isSymbolicLink() || sb.isSymbolicLink()) {
    return sa.isSymbolicLink() && sb.isSymbolicLink() && fs.readlinkSync(a) === fs.readlinkSync(b);
  }
  if (sa.size !== sb.size) return false;
  return fs.readFileSync(a).equals(fs.readFileSync(b));
}

/** Writes `source` to `target` atomically (temp file + rename): a reader sees old or new, never half. */
function placeFile(source, target) {
  fs.mkdirSync(path.dirname(target), {recursive: true});
  const tmp = `${target}.loom-tmp-${process.pid}`;
  const st = fs.lstatSync(source);
  if (st.isSymbolicLink()) {
    fs.rmSync(tmp, {force: true});
    fs.symlinkSync(fs.readlinkSync(source), tmp);
  } else {
    fs.copyFileSync(source, tmp);
    fs.chmodSync(tmp, st.mode);
  }
  fs.renameSync(tmp, target);
}

/** Writes `content` to `target` atomically, only when it differs. */
function placeContent(target, content) {
  try {
    if (fs.readFileSync(target, 'utf8') === content) return;
  } catch {}
  fs.mkdirSync(path.dirname(target), {recursive: true});
  const tmp = `${target}.loom-tmp-${process.pid}`;
  fs.writeFileSync(tmp, content);
  fs.renameSync(tmp, target);
}

// ---- Package managers -----------------------------------------------------------------------------

function assertSupportedLinker(projectRoot) {
  for (let dir = projectRoot; ; dir = path.dirname(dir)) {
    if (fs.existsSync(path.join(dir, '.pnp.cjs')) || fs.existsSync(path.join(dir, '.pnp.js'))) {
      throw new Error(
        'loom: Yarn Plug\'n\'Play is not supported (React Native\'s native builds need a node_modules tree). ' +
          'Set `nodeLinker: node-modules` in .yarnrc.yml.',
      );
    }
    if (path.dirname(dir) === dir) return;
  }
}

/** The directory `name` resolves to from `fromDir`, or null. */
function resolvePackageDir(name, fromDir) {
  for (let dir = fromDir; ; dir = path.dirname(dir)) {
    const candidate = path.join(dir, 'node_modules', name);
    if (fs.existsSync(path.join(candidate, 'package.json'))) return fs.realpathSync(candidate);
    if (path.dirname(dir) === dir) return null;
  }
}

/**
 * React Native's own dependencies, as the pristine package resolves them. Under pnpm (strict, not
 * hoisted) they sit next to the package in the store, not under the app's node_modules, so the copy
 * would no longer find them: each one the copy cannot resolve the same way is linked into
 * <tree>/node_modules. With npm/yarn hoisting nothing needs a link.
 */
function dependencyLinks(upstreamDir, dest) {
  const pkg = JSON.parse(fs.readFileSync(path.join(upstreamDir, 'package.json'), 'utf8'));
  const links = {};
  for (const name of Object.keys({...pkg.dependencies, ...pkg.optionalDependencies})) {
    const fromUpstream = resolvePackageDir(name, upstreamDir);
    if (fromUpstream == null) continue;
    if (resolvePackageDir(name, path.dirname(dest)) === fromUpstream) continue;
    links[path.join('node_modules', name)] = fromUpstream;
  }
  return links;
}

// ---- Materialize ----------------------------------------------------------------------------------

/**
 * Build node_modules/.loom/react-native = pristine react-native + overrides.
 *
 * node_modules/react-native is never modified: it stays the 3-way merge base
 * and the reference `validate` hashes against. Every consumer (Gradle, CocoaPods,
 * Metro, codegen) is pointed at the materialized copy by the integration helpers.
 *
 * The tree is updated in place, file by file (each write atomic), under a lock: a build that reads
 * it meanwhile sees each file old or new, never a missing tree. Only files materialize itself wrote
 * are ever removed, so build outputs inside the tree (Gradle, CMake, Hermes downloads) survive.
 */
function materialize({projectRoot, force = false, out, log = console.log}) {
  assertSupportedLinker(projectRoot);
  const manifest = loadManifest();
  const upstream = findInstalledReactNative(projectRoot);
  const {errors, warnings} = validateDetailed({upstreamDir: upstream.dir, upstreamVersion: upstream.version, manifest});
  if (errors.length) {
    throw new Error('Refusing to materialize:\n  ' + errors.join('\n  '));
  }
  for (const w of warnings) log(`loom: warning: ${w}`);

  const dest = out ? path.resolve(out) : materializedDir(projectRoot);
  const switches = readGradleSwitches(projectRoot);
  const stamp = computeStamp(manifest, upstream);
  return withLock(path.dirname(dest), log, () => {
    const stampFile = path.join(dest, STAMP);
    let upToDate = false;
    if (!force && fs.existsSync(stampFile)) {
      try {
        upToDate = JSON.parse(fs.readFileSync(stampFile, 'utf8')).hash === stamp.hash;
      } catch {}
    }
    if (!upToDate) {
      syncTree({manifest, upstream, dest});
    }
    writeSwitches(upstream, dest, switches);
    placeContent(stampFile, JSON.stringify({...stamp, gradleSwitches: switches}, null, 2) + '\n');
    log(
      upToDate
        ? `loom: ${path.relative(projectRoot, dest)} is up to date`
        : `loom: materialized react-native@${upstream.version} + ${manifest.overrides.length} overrides -> ${path.relative(projectRoot, dest)}`,
    );
    return dest;
  });
}

function syncTree({manifest, upstream, dest}) {
  // The desired tree: relative path -> source file.
  const desired = new Map();
  for (const rel of listFiles(upstream.dir)) desired.set(rel, path.join(upstream.dir, rel));
  for (const o of manifest.overrides) {
    const rel = path.normalize(o.file);
    if (o.monorepoOnly && !desired.has(rel)) continue; // monorepo build tooling, not in the npm package
    if (o.type === 'patch' || o.type === 'platform') desired.set(rel, overridePath(o));
    else if (o.type === 'delete') desired.delete(rel);
  }
  // Owned by `writeSwitches` (React Native's file plus the app's switches).
  desired.delete('gradle.properties');
  for (const [rel, target] of Object.entries(dependencyLinks(upstream.dir, dest))) {
    desired.set(rel, {link: target});
  }

  fs.mkdirSync(dest, {recursive: true});
  for (const [rel, source] of desired) {
    const target = path.join(dest, rel);
    if (typeof source === 'object') {
      let current = null;
      try {
        current = fs.readlinkSync(target);
      } catch {}
      if (current !== source.link) {
        fs.mkdirSync(path.dirname(target), {recursive: true});
        fs.rmSync(target, {recursive: true, force: true});
        fs.symlinkSync(source.link, target, 'dir');
      }
      continue;
    }
    if (!sameFile(source, target)) placeFile(source, target);
  }

  // Remove what an earlier materialize wrote that is no longer wanted (an override dropped, a file
  // gone upstream). Never anything else: build outputs live in this tree too.
  const filesFile = path.join(dest, FILES);
  let previous = [];
  try {
    previous = JSON.parse(fs.readFileSync(filesFile, 'utf8'));
  } catch {}
  for (const rel of previous) {
    if (!desired.has(rel) && rel !== 'gradle.properties') fs.rmSync(path.join(dest, rel), {recursive: true, force: true});
  }
  placeContent(filesFile, JSON.stringify([...desired.keys()].sort()) + '\n');
}

/** React Native's gradle.properties plus the app's `loom.*` switches. Rewritten only when it changes. */
function writeSwitches(upstream, dest, switches) {
  let base = '';
  const upstreamProps = path.join(upstream.dir, 'gradle.properties');
  if (fs.existsSync(upstreamProps)) base = fs.readFileSync(upstreamProps, 'utf8');
  const overrideProps = path.join(PKG_ROOT, 'src', 'gradle.properties');
  if (fs.existsSync(overrideProps)) base = fs.readFileSync(overrideProps, 'utf8');
  const entries = Object.entries(switches);
  const block = entries.length
    ? `\n# Loom switches copied from android/gradle.properties by \`loom materialize\`\n${entries.map(([k, v]) => `${k}=${v}`).join('\n')}\n`
    : '';
  if (!base && !block) return;
  placeContent(path.join(dest, 'gradle.properties'), base + block);
}

module.exports = {materialize, materializedDir, readGradleSwitches, GRADLE_SWITCHES, STAMP};
