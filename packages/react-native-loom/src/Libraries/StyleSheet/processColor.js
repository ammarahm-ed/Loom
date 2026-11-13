/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

'use strict';

import type {ColorValue, NativeColorValue} from './StyleSheet';

import Platform from '../Utilities/Platform';

const normalizeColor = require('./normalizeColor').default;

export type ProcessedColorValue = number | NativeColorValue;

// Loom: a color string always processes to the same value, and apps use a
// small set of them over and over (every styled view re-processes its colors on
// every update). Parsing one is a series of regex matches; a lookup is not.
// Bounded so an app that generates colors cannot grow it without limit.
const STRING_COLOR_CACHE_LIMIT = 1024;
const stringColorCache: Map<string, ?ProcessedColorValue> = new Map();

/* eslint no-bitwise: 0 */
function processColor(color?: ?(number | ColorValue)): ?ProcessedColorValue {
  if (color === undefined || color === null) {
    return color;
  }
  if (typeof color === 'string') {
    const cached = stringColorCache.get(color);
    if (cached !== undefined || stringColorCache.has(color)) {
      return cached;
    }
    const processed = processColorUncached(color);
    if (stringColorCache.size < STRING_COLOR_CACHE_LIMIT) {
      stringColorCache.set(color, processed);
    }
    return processed;
  }
  return processColorUncached(color);
}

function processColorUncached(
  color: number | ColorValue,
): ?ProcessedColorValue {

  let normalizedColor = normalizeColor(color);
  if (normalizedColor === null || normalizedColor === undefined) {
    return undefined;
  }

  if (typeof normalizedColor === 'object') {
    const processColorObject =
      require('./PlatformColorValueTypes').processColorObject;

    const processedColorObj = processColorObject(normalizedColor);

    if (processedColorObj != null) {
      return processedColorObj;
    }
  }

  if (typeof normalizedColor !== 'number') {
    return null;
  }

  // Converts 0xrrggbbaa into 0xaarrggbb
  normalizedColor = ((normalizedColor << 24) | (normalizedColor >>> 8)) >>> 0;

  if (Platform.OS === 'android') {
    // Android use 32 bit *signed* integer to represent the color
    // We utilize the fact that bitwise operations in JS also operates on
    // signed 32 bit integers, so that we can use those to convert from
    // *unsigned* to *signed* 32bit int that way.
    normalizedColor = normalizedColor | 0x0;
  }
  return normalizedColor;
}

export default processColor;
