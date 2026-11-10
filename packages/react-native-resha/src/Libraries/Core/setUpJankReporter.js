/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict
 * @format
 */

'use strict';

/**
 * React Native X: debug-only support for the main-thread jank reporter.
 *
 * Native code measures every main-thread task and, when one runs longer than the budget, logs why
 * through `console.warn`. This module adds two JS-side pieces:
 *
 * - It remembers where `setTimeout`, `setInterval` and `requestAnimationFrame` callbacks were
 *   scheduled, so a slow callback can be traced to the line that registered it. The `Error` object
 *   is kept and its `stack` is read only if the callback turns out to be slow, so the cost per
 *   registration is the stack capture itself and nothing is formatted otherwise.
 * - It prints reports the Java side raises (slow frames, slow view applies) as `console.warn`.
 *
 * `__reshaJankBudgetMs` only exists when the native reporter is compiled in (debug builds with JS on
 * the main thread); without it only the Java-side reports are printed.
 */

const budgetFn = global.__reshaJankBudgetMs;

if (typeof budgetFn === 'function') {
  const budget: number = budgetFn();
  const note = global.__reshaJankNote;

  if (budget > 0 && typeof note === 'function') {
    const siteOf = (error: Error): string => {
      const lines = String(error.stack ?? '').split('\n');
      // Skip the message, this wrapper's frame and the wrapper's callee; the next frame is the caller.
      const frame = lines.find(
        (line, index) => index > 1 && !line.includes('setUpJankReporter'),
      );
      // Metro's bundle URL carries a long query string; keep the bundle name and position.
      return frame != null
        ? frame
            .trim()
            .replace(/^at /, '')
            .replace(/\/\/?[?&]platform=[^:)\s]*/, '')
        : 'unknown';
    };
    for (const name of ['setTimeout', 'setInterval', 'requestAnimationFrame']) {
      const original = global[name];
      if (typeof original !== 'function') {
        continue;
      }
      global[name] = function (callback: mixed, ...rest: Array<mixed>) {
        if (typeof callback !== 'function') {
          return original.call(this, callback, ...rest);
        }
        const scheduled = new Error();
        const wrapped = function (...args: Array<mixed>) {
          const start = performance.now();
          try {
            // $FlowFixMe[incompatible-use]
            return callback.apply(this, args);
          } finally {
            const elapsed = performance.now() - start;
            if (elapsed >= budget) {
              note(name, siteOf(scheduled), elapsed);
            }
          }
        };
        return original.call(this, wrapped, ...rest);
      };
    }
  }
}

// The native reporter hands its message here instead of calling console.warn itself, so the bundle
// positions in it (`name (index.bundle:12:3)`) can be mapped to source files through Metro's
// /symbolicate endpoint, the same one LogBox uses. If Metro is not reachable the raw message is
// printed. LogBox cannot be fed a custom stack for a warning, so the message text is rewritten.
global.__reshaJankReport = (message: string): void => {
  const position = /\(([^()\s]*?):(\d+):(\d+)\)/g;
  const frames = [];
  let match;
  while ((match = position.exec(message)) != null) {
    frames.push({
      text: match[0],
      methodName: '',
      file: match[1],
      lineNumber: Number(match[2]),
      column: Number(match[3]),
    });
  }
  if (frames.length === 0) {
    console.warn(message);
    return;
  }
  let scriptURL = null;
  try {
    scriptURL =
      require('../NativeModules/specs/NativeSourceCode').default.getConstants()
        .scriptURL;
  } catch {}
  const symbolicate = require('./Devtools/symbolicateStackTrace').default;
  const request = frames.map(frame => ({
    methodName: frame.methodName,
    file: scriptURL ?? frame.file,
    lineNumber: frame.lineNumber,
    column: frame.column,
  }));
  symbolicate(request)
    .then(result => {
      let text = message;
      result.stack.forEach((mapped, index) => {
        const file = mapped.file ?? '';
        if (file !== '' && !file.includes('index.bundle')) {
          const short = file.replace(/^.*\/(?=(?:src|node_modules|apps|packages)\/)/, '');
          text = text.replace(
            frames[index].text,
            `(${short}:${mapped.lineNumber}:${mapped.column})`,
          );
        }
      });
      console.warn(text);
    })
    .catch(() => console.warn(message));
};

// Reports raised by the Java side (slow frames, slow view applies), in either thread mode.
require('../EventEmitter/RCTDeviceEventEmitter').default.addListener(
  'ReshaJank',
  (message: string) => {
    console.warn(message);
  },
);
