import {NativeModules} from 'react-native';

export type Check = boolean | number | string;
export type Result = {lib: string; pass: boolean; checks: Record<string, Check>};

// Results go to the dev server that served this bundle (metro.config.js handles /compat-report).
function collectorUrl(): string {
  try {
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const getDevServer = require('react-native/Libraries/Core/Devtools/getDevServer').default;
    return `${String(getDevServer().url).replace(/\/$/, '')}/compat-report`;
  } catch {
    return 'http://localhost:8088/compat-report';
  }
}

const listeners = new Set<(r: Result) => void>();
export function onReport(fn: (r: Result) => void) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

// Each library screen calls report() once its checks are done. Prints one [compat] line and posts
// it to the dev server (see collectorUrl).
export function report(lib: string, checks: Record<string, Check>) {
  const pass = Object.values(checks).every(v => v !== false);
  const result = {lib, pass, checks};
  const line = `[compat] ${JSON.stringify(result)}`;
  console.log(line);
  NativeModules.LoomBuildConfig?.log?.(line);
  fetch(collectorUrl(), {method: 'POST', body: JSON.stringify(result)}).catch(() => {});
  // Release builds have no dev server: scripts/collect.js listens here (iOS simulator shares the host network).
  fetch('http://localhost:8099/', {method: 'POST', body: JSON.stringify(result)}).catch(() => {});
  listeners.forEach(fn => fn(result));
}

export const wait = (ms: number) => new Promise(r => setTimeout(r, ms));
export const near = (a: number | undefined, b: number, tol = 1) =>
  a !== undefined && Math.abs(a - b) <= tol;
