'use strict';

/**
 * Loom's runtime settings. See index.d.ts for the settings and
 * website/reference/configuration.md for what each one does.
 *
 *   import {Loom} from 'react-native-loom';
 *   Loom.asyncText = false;
 *   Loom.jankBudgetMs = 30;
 *   Loom.jsOnMainThread; // read-only build info
 *
 * Under the Loom renderer the values live in native atomics (the registry in
 * LoomSettings.h), reached through `nativeFabricUIManager.loomSettings`. Under
 * stock React Native (Fabric) there is nothing to configure: `renderer` is
 * 'fabric', the others report their defaults, and writes do nothing (with a
 * development warning).
 */

const version = require('./package.json').version;

// name -> [default on stock RN]. Order is the documented order.
const WRITABLE = {
  asyncText: true,
  asyncTextFirstDraw: true,
  asyncTextResize: true,
  earlyCreate: false,
  parallelText: false,
  jankReporter: true,
  jankBudgetMs: 50,
  profileWorker: false,
};
const READ_ONLY = ['renderer', 'jsOnMainThread', 'worker', 'version'];

let cached = null;

/** The native settings object, or null under stock Fabric. Looked up lazily: the binding may install after this module loads. */
function host() {
  if (cached != null) {
    return cached;
  }
  const ui = global.nativeFabricUIManager;
  const settings = ui != null ? ui.loomSettings : null;
  if (settings != null && typeof settings === 'object') {
    cached = settings;
  }
  return cached;
}

const warned = new Set();
function warnInert(name) {
  if (__DEV__ && !warned.has(name)) {
    warned.add(name);
    console.warn(
      `Loom.${name} was set, but this app is not running the Loom renderer; the setting has no effect.`,
    );
  }
}

const Loom = {};

Object.keys(WRITABLE).forEach(name => {
  Object.defineProperty(Loom, name, {
    enumerable: true,
    get() {
      const native = host();
      return native != null ? native[name] : WRITABLE[name];
    },
    set(value) {
      const native = host();
      if (native == null) {
        warnInert(name);
        return;
      }
      native[name] = value;
    },
  });
});

READ_ONLY.forEach(name => {
  Object.defineProperty(Loom, name, {
    enumerable: true,
    get() {
      if (name === 'version') {
        return version;
      }
      const native = host();
      if (name === 'renderer') {
        return native != null ? native.renderer : 'fabric';
      }
      return native != null ? native[name] : false;
    },
    set(_value) {
      throw new TypeError(`Loom.${name} is read-only`);
    },
  });
});

module.exports = {Loom};
