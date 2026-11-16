'use strict';

const {spawnSync} = require('child_process');
const fs = require('fs');
const path = require('path');
const {loadManifest, overridePath} = require('./manifest');

function diffStat(a, b) {
  const r = spawnSync('git', ['diff', '--no-index', '--numstat', a, b], {encoding: 'utf8'});
  const m = /^(\d+|-)\t(\d+|-)/.exec(r.stdout);
  return m ? {add: +m[1] || 0, del: +m[2] || 0} : {add: 0, del: 0};
}

function lineCount(f) {
  return fs.readFileSync(f, 'utf8').split('\n').length;
}

/**
 * Per-file divergence of every patch override: how much of the upstream file Loom changed. A file
 * Loom rewrote most of costs a hand merge on every React Native upgrade; those are the candidates to
 * turn into a small patch plus a Loom-only file or hook.
 */
function divergence({upstreamDir, log = console.log, limit = Infinity}) {
  const manifest = loadManifest();
  const rows = [];
  for (const o of manifest.overrides) {
    if (o.type !== 'patch') continue;
    const up = path.join(upstreamDir, o.file);
    if (!fs.existsSync(up)) continue;
    const s = diffStat(up, overridePath(o));
    const lines = lineCount(up);
    rows.push({file: o.file, lines, add: s.add, del: s.del, pct: Math.round((100 * Math.max(s.add, s.del)) / Math.max(lines, 1))});
  }
  rows.sort((a, b) => b.add + b.del - (a.add + a.del));
  log('changed%  upstream   +lines   -lines  file');
  for (const r of rows.slice(0, limit)) {
    log(`${String(r.pct).padStart(7)}% ${String(r.lines).padStart(9)} ${String(r.add).padStart(8)} ${String(r.del).padStart(8)}  ${r.file}`);
  }
  const heavy = rows.filter(r => r.pct >= 50).length;
  log(`\n${rows.length} patches; ${heavy} change half or more of their upstream file.`);
  return rows;
}

/** Summary of how much of RN we carry, grouped by area. */
function status({upstreamDir, log = console.log}) {
  const manifest = loadManifest();
  const byArea = new Map();
  for (const o of manifest.overrides) {
    const a = byArea.get(o.area) || {files: 0, add: 0, del: 0, types: {}};
    a.files++;
    a.types[o.type] = (a.types[o.type] || 0) + 1;
    if (o.type === 'patch') {
      const s = diffStat(path.join(upstreamDir, o.file), overridePath(o));
      a.add += s.add;
      a.del += s.del;
    } else if (o.type === 'platform' || o.type === 'test') {
      a.add += lineCount(overridePath(o));
    }
    byArea.set(o.area, a);
  }
  log(`react-native@${manifest.upstream.version}: ${manifest.overrides.length} overrides\n`);
  log('area               files    +lines   -lines  types');
  let tf = 0, ta = 0, td = 0;
  for (const [area, a] of [...byArea].sort((x, y) => y[1].add - x[1].add)) {
    tf += a.files; ta += a.add; td += a.del;
    const types = Object.entries(a.types).map(([t, n]) => `${t}:${n}`).join(' ');
    log(`${area.padEnd(18)} ${String(a.files).padStart(5)} ${String(a.add).padStart(9)} ${String(a.del).padStart(8)}  ${types}`);
  }
  log(`${'total'.padEnd(18)} ${String(tf).padStart(5)} ${String(ta).padStart(9)} ${String(td).padStart(8)}`);
}

/** Unified diff of patches against pristine upstream. */
function diff({upstreamDir, files, log = console.log}) {
  const manifest = loadManifest();
  for (const o of manifest.overrides) {
    if (o.type !== 'patch') continue;
    if (files.length && !files.some(f => o.file.includes(f))) continue;
    const r = spawnSync(
      'git',
      ['diff', '--no-index', '--src-prefix=upstream/', '--dst-prefix=loom/', path.join(upstreamDir, o.file), overridePath(o)],
      {encoding: 'utf8'},
    );
    log(r.stdout);
  }
}

module.exports = {status, diff, divergence};
