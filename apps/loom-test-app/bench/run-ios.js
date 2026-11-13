#!/usr/bin/env node
/**
 * iOS device benchmark: stock React Native vs Loom (JS on the main thread) vs Loom (JS on its own
 * thread). The counterpart of run.js, driven by `xcrun devicectl` instead of adb.
 *
 * Build the three Release apps first (bench/build-ios.sh does it and leaves them in <DD_ROOT>/products, pass that as --products; each gets its own bundle id, so
 * all of them stay installed side by side):
 *
 *   bench/build-ios.sh stock     # com.loomtestapp.bench.stock  stock React Native, prebuilt core
 *   bench/build-ios.sh on        # com.loomtestapp.bench.on     Loom renderer + worker, JS on main
 *   bench/build-ios.sh js        # com.loomtestapp.bench.js     Loom renderer + worker, JS on its own thread
 *
 * then, with the phone unlocked and awake:
 *
 *   node bench/run-ios.js --device <devicectl id> [--variants stock,on,js] [--scenarios a,b,...]
 *     [--launches 5] [--launch-args "-LoomNoRafScope YES"] [--out <repo>/.local/bench/ios-<timestamp>.jsonl] [--products <dir>]
 *
 * Each scenario runs once per variant (a cold launch of the scenario, `-scenario <name>` as a
 * launch argument; AppDelegate.swift hands it to Bench.tsx). The only repeated case is `startup`,
 * which is launched --launches times and reported as the median of the first-content time.
 *
 * What is measured, and where it comes from (all lines carry the prefix `LoomBench `, printed by
 * ios/LoomTestApp/BenchProbe.mm to the device console and read through `devicectl --console`):
 *   ready sinceStart=N   ms from process fork to the first CADisplayLink frame after the view with
 *                        nativeID bench-ready exists
 *   frames ...           CADisplayLink frame-interval statistics over the scenario's window
 *   js [bench] result {} the JS-reported result of the burst and heavy scenarios
 *
 * Scrolling is driven from JS (Bench.tsx useAutoScroll) because iOS has no `input swipe`. Tap
 * latency is not measured on iOS: devicectl cannot inject touches.
 */

'use strict';

const { execFileSync, spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const REPO = path.resolve(__dirname, '../../..');
const ALL_SCENARIOS = [
  'startup',
  'burst-raf',
  'burst-unique',
  'burst-resize',
  'burst-resize-once',
  'burst-small',
  'burst-timeout',
  'transition-heavy',
  'heavy-sync',
  'anim',
  'anim-busy',
  'js-anim',
  'js-anim-busy',
  'scroll',
  'scroll-busy',
  'memory',
];

function parseArgs(argv) {
  const args = {
    variants: ['stock', 'on', 'js'],
    scenarios: ALL_SCENARIOS,
    launches: 5,
    launchArgs: [],
    products: null,
    out: path.join(REPO, '.local/bench', `ios-${new Date().toISOString().replace(/[:.]/g, '-')}.jsonl`),
  };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i];
    const value = argv[++i];
    if (key === '--device') args.device = value;
    else if (key === '--variants') args.variants = value.split(',');
    else if (key === '--scenarios') args.scenarios = value.split(',');
    else if (key === '--launch-args') args.launchArgs = value.split(' ').filter(Boolean);
    else if (key === '--launches') args.launches = Number(value);
    else if (key === '--out') args.out = value;
    else if (key === '--products') args.products = value;
    else if (key === '--no-install') {
      args.noInstall = true;
      i--;
    } else throw new Error(`unknown argument ${key}`);
  }
  if (!args.device) throw new Error('--device is required');
  return args;
}

const args = parseArgs(process.argv.slice(2));
const bundle = variant => `com.loomtestapp.bench.${variant}`;
const sleep = ms => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);

function devicectl(...cmd) {
  return execFileSync('xcrun', ['devicectl', ...cmd], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    maxBuffer: 64 * 1024 * 1024,
    // A stalled device call must fail the case, not block the run (installs can take minutes).
    timeout: cmd[1] === 'install' ? 300_000 : 120_000,
    killSignal: 'SIGKILL',
  });
}

function appPath(variant) {
  const dir = args.products || path.join(__dirname, '../ios/build-bench');
  return path.join(dir, variant, 'LoomTestApp.app');
}

function installAll() {
  for (const variant of args.variants) {
    console.log(`installing ${variant} (${bundle(variant)})`);
    devicectl('device', 'install', 'app', '--device', args.device, appPath(variant));
  }
}

// Pids of the bench apps that are running, so a case always starts from a cold process and no other
// variant is left competing for the CPU.
function terminateBenchApps() {
  const tmp = path.join(require('os').tmpdir(), `loom-bench-procs-${process.pid}.json`);
  try {
    devicectl('device', 'info', 'processes', '--device', args.device, '--json-output', tmp);
    const json = JSON.parse(fs.readFileSync(tmp, 'utf8'));
    const running = json.result?.runningProcesses ?? [];
    // The executable of every variant is .../com.loomtestapp.bench.<v>-not-in-path, so match by app
    // container: ask devicectl for each variant's install URL.
    const urls = args.variants.map(v => installURL(v)).filter(Boolean);
    for (const proc of running) {
      const exe = proc.executable ?? '';
      if (urls.some(url => exe.startsWith(url))) {
        try {
          devicectl('device', 'process', 'terminate', '--device', args.device, '--pid', String(proc.processIdentifier));
        } catch {}
      }
    }
  } catch (error) {
    console.log(`could not list processes: ${error.message.split('\n')[0]}`);
  }
}

const installURLs = {};
function installURL(variant) {
  if (installURLs[variant]) return installURLs[variant];
  const tmp = path.join(require('os').tmpdir(), `loom-bench-apps-${process.pid}.json`);
  devicectl('device', 'info', 'apps', '--device', args.device, '--bundle-id', bundle(variant), '--json-output', tmp);
  const json = JSON.parse(fs.readFileSync(tmp, 'utf8'));
  const url = json.result?.apps?.[0]?.url;
  if (url) installURLs[variant] = url.replace(/^file:\/\//, '');
  return installURLs[variant];
}

/**
 * Launches the variant with `-scenario <scenario>` attached to the console and collects its
 * output until `done(text)` returns true or the timeout passes. Resolves with the text.
 */
function launch(variant, scenario, done, timeoutMs) {
  terminateBenchApps();
  return new Promise((resolve, reject) => {
    const child = spawn(
      'xcrun',
      ['devicectl', 'device', 'process', 'launch', '--device', args.device, '--terminate-existing', '--console', bundle(variant), '--', '-scenario', scenario, '-autoscroll', ...args.launchArgs],
      { stdio: ['ignore', 'pipe', 'pipe'] },
    );
    let text = '';
    const finish = (error) => {
      clearTimeout(timer);
      child.kill('SIGINT');
      error ? reject(error) : resolve(text);
    };
    const onData = chunk => {
      text += chunk.toString();
      if (text.includes('Unable to launch') || /device is locked|Locked/.test(text.split('\n').slice(0, 20).join('\n'))) {
        if (/locked/i.test(text)) return finish(new Error(`launch failed (is the phone unlocked?):\n${text.slice(0, 400)}`));
      }
      if (done(text)) finish();
    };
    child.stdout.on('data', onData);
    child.stderr.on('data', onData);
    child.on('exit', () => {
      if (!done(text)) finish(new Error(`app exited before the case finished:\n${text.slice(-600)}`));
    });
    const timer = setTimeout(() => finish(new Error(`timed out; output so far:\n${text.slice(-600)}`)), timeoutMs);
  });
}

const num = (text, re) => {
  const m = text.match(re);
  return m ? Number(m[1]) : NaN;
};

function parseFrames(text) {
  const line = text.split('\n').find(l => /LoomBench frames /.test(l));
  if (!line) return {};
  const get = key => num(line, new RegExp(`${key}=([\\d.]+)`));
  return {
    frames: get('n'),
    jankPct: get('jankPct'),
    p50Ms: get('p50'),
    p90Ms: get('p90'),
    p95Ms: get('p95'),
    p99Ms: get('p99'),
    maxMs: get('max'),
    over32: get('over32'),
  };
}

const hasFrames = text => /LoomBench frames /.test(text);

// CPU time over the measurement window (BenchProbe prints it just before the frames line).
function parseCpu(text) {
  const line = text.split('\n').find(l => /LoomBench cpu /.test(l));
  if (!line) return {};
  const get = key => num(line, new RegExp(`${key}=([\\d.]+)`));
  return {
    mainCpuMs: get('mainMs'),
    jsCpuMs: get('jsMs'),
    workerCpuMs: get('workerMs'),
    totalCpuMs: get('totalMs'),
    cpuWindowMs: get('windowMs'),
    ...parseText(text),
  };
}
// Loom builds: asynchronous text display counters since launch (see RCTParagraphComponentView.mm).
function parseText(text) {
  const line = text.split('\n').find(l => /LoomBench text /.test(l));
  if (!line) return {};
  const out = {};
  for (const [, key, value] of line.matchAll(/(\w+)=(\d+)/g)) out[`text_${key}`] = Number(value);
  return { text: out };
}
const hasResult = text => /\[bench\] result (\{.*\})/.test(text);

async function runScenario(variant, scenario) {
  const base = scenario.replace(/-busy$/, '');
  if (base === 'startup') {
    // Cold starts only (devicectl --terminate-existing); prewarmed or background launches are
    // dropped and retried, as native-measure-tti.md says. Milestones are ms since process start.
    const keys = ['nativeLaunchEnd', 'appCreationEnd', 'runJSBundleEnd', 'contentAppeared', 'screenInteractive', 'mounted', 'commitStart', 'displayed'];
    const launches = [];
    for (let attempt = 0; launches.length < args.launches && attempt < args.launches * 2; attempt++) {
      const text = await launch(variant, 'startup', t => /LoomBench startup .*foreground=\d/.test(t), 30000);
      const line = text.match(/LoomBench startup (.*)/)[1];
      const m = Object.fromEntries([...line.matchAll(/(\w+)=([-\d.]+)/g)].map(([, k, v]) => [k, Number(v)]));
      if (m.prewarm || !m.foreground) continue;
      launches.push(m);
    }
    const out = { launches: launches.map(m => m.screenInteractive) };
    for (const k of keys) out[`${k}Ms`] = median(launches.map(m => m[k]));
    out.ttiMs = out.screenInteractiveMs;
    return out;
  }
  if (base.startsWith('memory')) {
    const text = await launch(variant, scenario, hasResult, 180000);
    const { scenario: _s, ...rest } = JSON.parse(text.match(/\[bench\] result (\{.*\})/)[1]);
    return rest;
  }
  if (base.startsWith('burst')) {
    const text = await launch(variant, scenario, t => hasResult(t) && hasFrames(t), 180000);
    const { totalMs, perCommitMs } = JSON.parse(text.match(/\[bench\] result (\{.*\})/)[1]);
    return { totalMs, perCommitMs, ...parseCpu(text) };
  }
  if (base === 'transition-heavy' || base === 'heavy-sync') {
    const text = await launch(variant, scenario, t => hasResult(t) && hasFrames(t), 90000);
    const { landMs, landP90Ms } = JSON.parse(text.match(/\[bench\] result (\{.*\})/)[1]);
    return { ...parseFrames(text), ...parseCpu(text), landMs, landP90Ms };
  }
  if (base === 'anim' || base === 'js-anim' || base === 'scroll') {
    const text = await launch(variant, scenario, hasFrames, 60000);
    return { ...parseFrames(text), ...parseCpu(text) };
  }
  throw new Error(`unknown scenario ${scenario}`);
}

function quantile(values, q) {
  const sorted = values.filter(v => Number.isFinite(v)).sort((a, b) => a - b);
  if (sorted.length === 0) return NaN;
  const pos = (sorted.length - 1) * q;
  const lo = Math.floor(pos);
  const hi = Math.ceil(pos);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - lo);
}
const median = values => quantile(values, 0.5);

function summarize(samples) {
  const round = v => (Number.isFinite(v) ? Math.round(v * 10) / 10 : 'n/a');
  const rows = [`| scenario | metric | ${args.variants.join(' | ')} |`, `|---|---|${args.variants.map(() => '---').join('|')}|`];
  const keys = new Map();
  for (const s of samples) {
    for (const [metric, value] of Object.entries(s.result)) {
      if (Array.isArray(value)) continue;
      const key = `${s.scenario}\t${metric}`;
      if (!keys.has(key)) keys.set(key, {});
      keys.get(key)[s.variant] = value;
    }
  }
  for (const [key, byVariant] of keys) {
    const [scenario, metric] = key.split('\t');
    rows.push(`| ${scenario} | ${metric} | ${args.variants.map(v => (v in byVariant ? round(byVariant[v]) : '—')).join(' | ')} |`);
  }
  return rows.join('\n');
}

async function main() {
  fs.mkdirSync(path.dirname(args.out), { recursive: true });
  if (!args.noInstall) installAll();
  const samples = [];
  for (const scenario of args.scenarios) {
    for (const variant of args.variants) {
      let result;
      try {
        result = await runScenario(variant, scenario);
      } catch (error) {
        console.log(`${scenario} ${variant}: FAILED ${error.message}`);
        continue;
      }
      const sample = { scenario, variant, result };
      samples.push(sample);
      fs.appendFileSync(args.out, `${JSON.stringify(sample)}\n`);
      console.log(`${scenario} ${variant}: ${JSON.stringify(result)}`);
    }
  }
  terminateBenchApps();
  console.log(`\n${summarize(samples)}\n\nraw samples: ${args.out}`);
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
