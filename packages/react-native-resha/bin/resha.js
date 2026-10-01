#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const {loadManifest, saveManifest, sha256File, SRC_DIR} = require('../lib/manifest');
const {classify} = require('../lib/areas');
const {materialize} = require('../lib/materialize');
const {validate} = require('../lib/validate');
const {upgrade} = require('../lib/upgrade');
const {importFork} = require('../lib/importFork');
const {status, diff} = require('../lib/inspect');
const {overlay} = require('../lib/overlay');
const {findInstalledReactNative, fetchUpstream} = require('../lib/upstream');

const USAGE = `resha <command> [options]

App commands (run from the app root):
  materialize [--force] [--out <dir>]   build node_modules/.resha/react-native
  validate                              check overrides against installed react-native

Maintainer commands (run inside the react-native-resha repo):
  status                                overrides by area, lines carried
  diff [path-substring...]              unified diff of patches vs upstream
  add <rn-relative-path> [--area a]     start overriding an upstream file
  add --platform <rn-relative-path>     register a new Resha-only file
  upgrade --to <version>                3-way merge overrides onto a new RN
  overlay --monorepo <dir> [--no-rn-tester]
                                        write overrides (+ Android tests, rn-tester
                                        benchmarks) into a facebook/react-native checkout
  import-fork --fork <dir> --base <sha> [--version v]
                                        capture a checkout's working tree back into
                                        overrides (inverse of overlay)

Common: --upstream <dir> uses that react-native package instead of fetching it.
`;

function parse(argv) {
  const args = {_: []};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const k = a.slice(2);
      const next = argv[i + 1];
      if (next === undefined || next.startsWith('--')) args[k] = true;
      else args[k] = argv[++i];
    } else args._.push(a);
  }
  return args;
}

function upstreamFor(args) {
  if (args.upstream) return path.resolve(args.upstream);
  const m = loadManifest();
  try {
    const installed = findInstalledReactNative(process.cwd());
    if (installed.version === m.upstream.version) return installed.dir;
  } catch {}
  return fetchUpstream(m.upstream.version, m.upstream.package);
}

function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  const args = parse(rest);
  switch (cmd) {
    case 'materialize':
      materialize({projectRoot: process.cwd(), force: !!args.force, out: args.out});
      break;
    case 'validate': {
      let upstreamDir, upstreamVersion;
      if (args.upstream) upstreamDir = path.resolve(args.upstream);
      else {
        const i = findInstalledReactNative(process.cwd());
        upstreamDir = i.dir;
        upstreamVersion = i.version;
      }
      const errors = validate({upstreamDir, upstreamVersion});
      if (errors.length) {
        console.error(`resha: ${errors.length} problem(s):\n  ` + errors.join('\n  '));
        process.exit(1);
      }
      console.log(`resha: ${loadManifest().overrides.length} overrides valid`);
      break;
    }
    case 'status':
      status({upstreamDir: upstreamFor(args)});
      break;
    case 'diff':
      diff({upstreamDir: upstreamFor(args), files: args._});
      break;
    case 'add': {
      const file = args.platform === true ? args._[0] : args.platform || args._[0];
      if (!file) throw new Error('add: missing path');
      const m = loadManifest();
      if (m.overrides.some(o => o.file === file)) throw new Error(`${file} is already an override`);
      const dest = path.join(SRC_DIR, file);
      fs.mkdirSync(path.dirname(dest), {recursive: true});
      const entry = {type: args.platform ? 'platform' : 'patch', file, area: args.area || classify(file)};
      if (entry.type === 'patch') {
        const up = path.join(upstreamFor(args), file);
        if (!fs.existsSync(up)) throw new Error(`${file} does not exist upstream; use --platform`);
        fs.copyFileSync(up, dest);
        entry.baseHash = sha256File(up);
      } else if (!fs.existsSync(dest)) {
        fs.writeFileSync(dest, '');
      }
      m.overrides.push(entry);
      saveManifest(m);
      console.log(`resha: added ${entry.type} override src/${file} [${entry.area}]`);
      break;
    }
    case 'upgrade':
      if (!args.to) throw new Error('upgrade: --to <version> required');
      if (upgrade({to: args.to}).conflicts.length) process.exit(2);
      break;
    case 'overlay':
      if (!args.monorepo) throw new Error('overlay: --monorepo <dir> required');
      overlay({monorepo: path.resolve(args.monorepo), withRnTester: !args['no-rn-tester']});
      break;
    case 'import-fork':
      if (!args.fork || !args.base) throw new Error('import-fork: --fork and --base required');
      importFork({
        fork: path.resolve(args.fork),
        base: args.base,
        version: args.version || loadManifest().upstream.version,
      });
      break;
    default:
      process.stdout.write(USAGE);
      process.exit(cmd ? 1 : 0);
  }
}

try {
  main();
} catch (e) {
  console.error(`resha: ${e.message}`);
  process.exit(1);
}
