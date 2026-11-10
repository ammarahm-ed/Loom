#!/usr/bin/env node
/**
 * Device benchmark: stock React Native vs Resha all-off vs Resha all-on.
 *
 * Build the three release APKs first (each gets its own application id, so all of them stay
 * installed and rounds can interleave them):
 *
 *   cd android
 *   RESHA_BENCH_STOCK=1 ./gradlew :app:assembleRelease -PbenchVariant=stock
 *   ./gradlew :app:assembleRelease -PbenchVariant=off -PRN_UI_THREAD=false -PUI_THREAD_RENDERER=false -PUI_THREAD_RENDERER_WORKER=false
 *   ./gradlew :app:assembleRelease -PbenchVariant=on  -PRN_UI_THREAD=true  -PUI_THREAD_RENDERER=true  -PUI_THREAD_RENDERER_WORKER=true
 *
 * (copying android/app/build/outputs/apk/release/app-release.apk aside after each), then:
 *
 *   node bench/run.js --serial <adb serial> --apk stock=<path> --apk off=<path> --apk on=<path> \
 *     [--rounds 10] [--scenarios startup,scroll,...] [--out results.jsonl]
 *
 * Every (round, scenario) runs all variants back to back, in an order rotated each round so drift
 * (heat, background work) is spread evenly. Raw samples go to --out as JSON lines; the summary
 * printed at the end is medians with the interquartile range.
 */

'use strict';

const { execFileSync } = require('child_process');
const fs = require('fs');

const ALL_SCENARIOS = [
  'startup',
  'scroll',
  'scroll-busy',
  'anim',
  'anim-busy',
  'js-anim',
  'js-anim-busy',
  'burst-raf',
  'burst-small',
  'transition-heavy',
  'heavy-sync',
  'burst-timeout',
  'latency',
  'latency-busy',
  'latency-heavy',
  'latency-snp',
  'latency-snp-busy',
];

function parseArgs(argv) {
  const args = { rounds: 10, scenarios: ALL_SCENARIOS, apks: {}, out: 'bench-results.jsonl' };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i];
    const value = argv[++i];
    if (key === '--serial') args.serial = value;
    else if (key === '--rounds') args.rounds = Number(value);
    else if (key === '--scenarios') args.scenarios = value.split(',');
    else if (key === '--out') args.out = value;
    else if (key === '--apk') {
      const [name, file] = value.split('=');
      args.apks[name] = file;
    } else if (key === '--no-install') {
      args.noInstall = true;
      i--;
    } else throw new Error(`unknown argument ${key}`);
  }
  if (!args.serial) throw new Error('--serial is required');
  if (Object.keys(args.apks).length === 0) throw new Error('at least one --apk name=path is required');
  return args;
}

const args = parseArgs(process.argv.slice(2));
const variants = Object.keys(args.apks);
const pkg = variant => `com.reshatestapp.bench.${variant}`;

function adb(...cmd) {
  return execFileSync('adb', ['-s', args.serial, ...cmd], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    maxBuffer: 64 * 1024 * 1024,
  });
}
const shell = command => adb('shell', command);
// For device preparation that some vendors refuse (OnePlus denies WRITE_SETTINGS and `svc power`).
function tryShell(command) {
  try {
    shell(command);
    return true;
  } catch {
    console.log(`device refused \`${command}\`; continuing without it`);
    return false;
  }
}
const sleep = ms => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);

// The pid of the app process launch() started. Logs are read for that process only: `logcat -c`
// does not reliably clear the buffer on every device (an Android 11 Sony kept the previous run's
// lines), and a stale `[bench] result` line would then be reported for the wrong build.
let currentPid = null;

function benchLog() {
  return adb(
    'logcat', '-d', '-v', 'raw', `--pid=${currentPid}`, '-s', 'ReshaBench:I', 'ReactNativeJS:I',
  );
}

function waitFor(pattern, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const match = benchLog().match(pattern);
    if (match) return match;
    sleep(250);
  }
  throw new Error(`timed out waiting for ${pattern}`);
}

function launch(variant, scenario) {
  for (const v of variants) shell(`am force-stop ${pkg(v)}`);
  adb('logcat', '-c');
  const out = shell(
    `am start -S -W -n ${pkg(variant)}/com.reshatestapp.MainActivity --es scenario ${scenario}`,
  );
  currentPid = shell(`pidof ${pkg(variant)}`).trim();
  const total = out.match(/TotalTime: (\d+)/);
  const ready = waitFor(/ready t=\d+ sinceStart=(\d+)/, 30000);
  return { ttidMs: total ? Number(total[1]) : NaN, firstContentMs: Number(ready[1]) };
}

function memory(variant) {
  const out = shell(`dumpsys meminfo ${pkg(variant)}`);
  const kb = re => {
    const m = out.match(re);
    return m ? Number(m[1]) : NaN;
  };
  return {
    pssKb: kb(/TOTAL PSS:\s+(\d+)/) || kb(/^\s*TOTAL\s+(\d+)/m),
    nativeHeapKb: kb(/^\s*Native Heap:\s+(\d+)/m),
    javaHeapKb: kb(/^\s*Java Heap:\s+(\d+)/m),
  };
}

function frames(variant) {
  const out = shell(`dumpsys gfxinfo ${pkg(variant)}`);
  const num = re => {
    const m = out.match(re);
    return m ? Number(m[1]) : NaN;
  };
  // Frames that took 32 ms or more (two or more dropped vsyncs at 60 Hz): steadier than jank %.
  let over32 = 0;
  const hist = out.match(/HISTOGRAM:(.*)/);
  if (hist) {
    for (const m of hist[1].matchAll(/(\d+)ms=(\d+)/g)) {
      if (Number(m[1]) >= 32) over32 += Number(m[2]);
    }
  }
  return {
    over32,
    frames: num(/Total frames rendered: (\d+)/),
    jankPct: num(/Janky frames: \d+ \(([\d.]+)%\)/),
    p50Ms: num(/50th percentile: (\d+)ms/),
    p90Ms: num(/90th percentile: (\d+)ms/),
    p95Ms: num(/95th percentile: (\d+)ms/),
    p99Ms: num(/99th percentile: (\d+)ms/),
  };
}

const resetFrames = variant => shell(`dumpsys gfxinfo ${pkg(variant)} reset`);

function scenarioStartup(variant) {
  const timing = launch(variant, 'startup');
  sleep(3000);
  return { ...timing, ...memory(variant) };
}

function scenarioScroll(variant, scenario) {
  launch(variant, scenario);
  sleep(1500);
  resetFrames(variant);
  for (let i = 0; i < 12; i++) {
    shell('input swipe 540 1900 540 500 120');
    sleep(500);
  }
  sleep(1000);
  return { ...frames(variant), ...memory(variant) };
}

function scenarioAnim(variant, scenario) {
  launch(variant, scenario);
  sleep(1500);
  resetFrames(variant);
  sleep(6000);
  return frames(variant);
}

function scenarioHeavy(variant, scenario) {
  launch(variant, scenario);
  sleep(1500);
  resetFrames(variant);
  const match = waitFor(/\[bench\] result (\{.*\})/, 60000);
  const { landMs, landP90Ms } = JSON.parse(match[1]);
  return { ...frames(variant), landMs, landP90Ms };
}

function scenarioBurst(variant, scenario) {
  launch(variant, scenario);
  const match = waitFor(/\[bench\] result (\{.*\})/, 120000);
  const { totalMs, perCommitMs } = JSON.parse(match[1]);
  return { totalMs, perCommitMs };
}

function scenarioLatency(variant, scenario) {
  launch(variant, scenario);
  sleep(1500);
  const taps = 30;
  for (let i = 0; i < taps; i++) {
    shell('input tap 540 350');
    sleep(350);
  }
  sleep(1500);
  const lines = benchLog().split('\n');
  const latencies = [];
  // Breakdown per tap: touch -> JS handler, JS handler -> commit done in JS, commit -> pre-draw.
  const toEvent = [];
  const toCommit = [];
  const toFrame = [];
  const toRender = [];
  let render = null;
  let down = null;
  let event = null;
  let commit = null;
  for (const line of lines) {
    const d = line.match(/^down t=(\d+)/);
    const p = line.match(/^probe alpha=[\d.]+ t=(\d+)/);
    const e = line.match(/\[bench\] mark event t=([\d.]+)/);
    const c = line.match(/\[bench\] mark commit t=([\d.]+)/);
    const r = line.match(/\[bench\] mark render t=([\d.]+)/);
    if (r) render = Number(r[1]);
    if (d) {
      down = Number(d[1]);
      event = commit = null;
    } else if (e) event = Number(e[1]);
    else if (c) commit = Number(c[1]);
    else if (p && down !== null) {
      const t = Number(p[1]);
      latencies.push(t - down);
      if (event !== null && commit !== null) {
        toEvent.push(event - down);
        toCommit.push(commit - event);
        toFrame.push(t - commit);
        if (render !== null) toRender.push(render - event);
      }
      render = null;
      down = event = commit = null;
    }
  }
  const round1 = v => Math.round(v * 10) / 10;
  return {
    taps,
    answered: latencies.length,
    latencyMs: median(latencies),
    latencyP90Ms: quantile(latencies, 0.9),
    touchToJsMs: round1(median(toEvent)),
    jsToCommitMs: round1(median(toCommit)),
    commitToFrameMs: round1(median(toFrame)),
    jsToRenderMs: toRender.length ? round1(median(toRender)) : null,
  };
}

function runScenario(variant, scenario) {
  const base = scenario.replace(/-busy$/, '');
  if (base === 'startup') return scenarioStartup(variant);
  if (base === 'scroll') return scenarioScroll(variant, scenario);
  if (base === 'anim' || base === 'js-anim') return scenarioAnim(variant, scenario);
  if (base === 'transition-heavy' || base === 'heavy-sync') return scenarioHeavy(variant, scenario);
  if (base.startsWith('burst')) return scenarioBurst(variant, scenario);
  if (base.startsWith('latency')) return scenarioLatency(variant, scenario);
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

function prepareDevice() {
  // Pin the panel to 120 Hz so a frame budget means the same thing on every run, and keep the
  // screen on for the whole session. Some vendors (OnePlus) refuse WRITE_SETTINGS to the shell, so
  // pinning is best-effort and the rate actually in use is printed instead.
  tryShell('settings put system peak_refresh_rate 120');
  tryShell('settings put system min_refresh_rate 120');
  const rate = shell('dumpsys display').match(/renderFrameRate ([\d.]+)/);
  console.log(`panel refresh rate: ${rate ? Math.round(Number(rate[1])) : 'unknown'} Hz`);
  // `true` covers AC, USB and wireless power; `usb` let a phone on a wall charger sleep and lock mid-run.
  tryShell('svc power stayon true') || tryShell('settings put global stay_on_while_plugged_in 7');
  tryShell('input keyevent KEYCODE_WAKEUP');
  tryShell('wm dismiss-keyguard');
  if (!args.noInstall) {
    for (const [variant, apk] of Object.entries(args.apks)) {
      console.log(`installing ${variant} from ${apk}`);
      installLikeStore(variant, apk);
    }
  }
}

// Puts the app in the compilation state a store install reaches: a clean install (no JIT profile
// left over from earlier runs), the APK's baseline profile installed by androidx.profileinstaller,
// then ahead-of-time compilation of what that profile lists. `adb install` alone leaves everything
// interpreted or JIT compiled, which makes the Java side of every measurement look slower than it is.
function installLikeStore(variant, apk) {
  tryShell(`pm uninstall ${pkg(variant)}`);
  adb('install', apk);
  tryShell(
    `am broadcast -a androidx.profileinstaller.action.INSTALL_PROFILE ` +
      `-n ${pkg(variant)}/androidx.profileinstaller.ProfileInstallReceiver`,
  );
  sleep(1500);
  shell(`am force-stop ${pkg(variant)}`);
  shell(`cmd package compile -f -m speed-profile ${pkg(variant)}`);
}

function summarize(samples) {
  const groups = new Map();
  for (const s of samples) {
    for (const [metric, value] of Object.entries(s.result)) {
      const key = `${s.scenario}\t${metric}`;
      if (!groups.has(key)) groups.set(key, {});
      (groups.get(key)[s.variant] ??= []).push(value);
    }
  }
  const fmt = values =>
    values
      ? `${round(median(values))} (${round(quantile(values, 0.25))}–${round(quantile(values, 0.75))})`
      : '—';
  const round = v => (Number.isFinite(v) ? Math.round(v * 10) / 10 : 'NaN');
  const rows = [`| scenario | metric | ${variants.join(' | ')} |`, `|---|---|${variants.map(() => '---').join('|')}|`];
  for (const [key, byVariant] of groups) {
    const [scenario, metric] = key.split('\t');
    rows.push(`| ${scenario} | ${metric} | ${variants.map(v => fmt(byVariant[v])).join(' | ')} |`);
  }
  return rows.join('\n');
}

function main() {
  prepareDevice();
  const samples = [];
  for (let round = 0; round < args.rounds; round++) {
    const order = variants.map((_, i) => variants[(i + round) % variants.length]);
    for (const scenario of args.scenarios) {
      for (const variant of order) {
        let result;
        try {
          result = runScenario(variant, scenario);
        } catch (error) {
          console.log(`round ${round} ${scenario} ${variant}: FAILED ${error.message}`);
          continue;
        }
        const sample = { round, scenario, variant, result };
        samples.push(sample);
        // Appended synchronously: the run loop blocks the event loop, so a stream would only flush
        // at exit and an interrupted run would lose every sample.
        fs.appendFileSync(args.out, `${JSON.stringify(sample)}\n`);
        console.log(`round ${round} ${scenario} ${variant}: ${JSON.stringify(result)}`);
      }
    }
  }
  for (const v of variants) shell(`am force-stop ${pkg(v)}`);
  console.log(`\n${summarize(samples)}`);
}

main();
