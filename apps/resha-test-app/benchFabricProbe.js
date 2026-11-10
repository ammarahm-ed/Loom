/**
 * Bench-only: times every call React's Fabric renderer makes into the native UI manager.
 *
 * Opt-in, because the timing itself costs about 1 ms per large update: only index.probe.js installs
 * it, before react-native binds the UI manager's methods (ReactFabric reads them once when it
 * loads). Build a probe APK with `ENTRY=index.probe.js bench/repack-js.sh ...`. Bench.tsx resets `global.__fabricStats` when
 * a tap reaches JS and logs it once React has committed: whatever part of that interval is not in
 * these calls is JavaScript, i.e. React reconciliation plus React Native's prop diffing.
 *
 * @format
 */

const METHODS = [
  'createNode',
  'cloneNodeWithNewChildren',
  'cloneNodeWithNewProps',
  'cloneNodeWithNewChildrenAndProps',
  'createChildSet',
  'appendChild',
  'appendChildToSet',
  'completeRoot',
];

// The renderer gets its UI manager from FabricUIManager.getFabricUIManager() when it loads. The
// global itself must stay the native host object (native code casts it back), so wrap what that
// function returns instead.
export function installFabricProbe() {
  const stats = {};
  global.__fabricStats = stats;
  const FabricUIManager = require('react-native/Libraries/ReactNative/FabricUIManager');
  const realGetFabricUIManager = FabricUIManager.getFabricUIManager;
  let wrapped = null;
  FabricUIManager.getFabricUIManager = function () {
    const target = realGetFabricUIManager();
    if (target == null) {
      return target;
    }
    if (wrapped == null) {
      wrapped = Object.create(target);
      for (const name of METHODS) {
        const fn = target[name];
        if (typeof fn !== 'function') {
          continue;
        }
        const entry = (stats[name] = { n: 0, ms: 0 });
        wrapped[name] = function (a, b, c, d, e) {
          const begin = performance.now();
          const result = fn(a, b, c, d, e);
          entry.n++;
          entry.ms += performance.now() - begin;
          return result;
        };
      }
    }
    return wrapped;
  };

  // React Native's JS-side prop diffing (props -> the update payload sent with each clone). The renderer
  // reads these through react-private-interface getters that re-require the module on every call, so
  // replacing the module's exports is enough.
  const payload = require('react-native/Libraries/ReactNative/ReactFabricPublicInstance/ReactNativeAttributePayload');
  for (const [name, key] of [
    ['create', 'createAttributePayload'],
    ['diff', 'diffAttributePayloads'],
  ]) {
    const fn = payload[name];
    if (typeof fn !== 'function') {
      continue;
    }
    const entry = (stats[key] = { n: 0, ms: 0, js: true });
    try {
      payload[name] = function (a, b, c) {
        const start = performance.now();
        const result = fn(a, b, c);
        entry.n++;
        entry.ms += performance.now() - start;
        return result;
      };
    } catch (e) {
      delete stats[key];
    }
  }
}

export function resetFabricStats() {
  const stats = global.__fabricStats;
  if (stats) {
    for (const key in stats) {
      stats[key].n = 0;
      stats[key].ms = 0;
    }
  }
}

export function fabricStatsSummary() {
  const stats = global.__fabricStats;
  if (!stats) {
    return null;
  }
  const out = {};
  let total = 0;
  for (const key in stats) {
    if (stats[key].n > 0) {
      out[key] = [stats[key].n, Number(stats[key].ms.toFixed(2))];
      if (!stats[key].js) {
        total += stats[key].ms;
      }
    }
  }
  out.nativeMs = Number(total.toFixed(2));
  return out;
}
