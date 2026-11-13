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

import VirtualizedLists from '@react-native/virtualized-lists';
import {startTransition} from 'react';

const BaseVirtualizedList = VirtualizedLists.VirtualizedList;

/**
 * Loom: true when JavaScript runs on the main (UI) thread. Native code answers
 * through the UI manager host object; anything else (stock Fabric, an older
 * binary) reads as false.
 */
function isJavaScriptOnMainThread(): boolean {
  try {
    // $FlowFixMe[prop-missing]
    return global.nativeFabricUIManager?.loomJsOnMain === true;
  } catch (e) {
    return false;
  }
}

/**
 * With JavaScript on the main thread a render pass blocks touches and frames
 * for as long as it runs. A scroll moves the render window by several rows at
 * once, and rendering those rows in one synchronous pass is what janks. The
 * window update is not urgent (the rows being added are off screen, and the
 * list already shows placeholders for them), so it is scheduled as a
 * transition: React then renders it in slices and yields to frames and input
 * between them.
 *
 * Only the windowing update (`_updateCellsToRender`) becomes a transition.
 * Layout, data and scroll-to updates keep their ordinary priority.
 */
// $FlowFixMe[incompatible-type]
class LoomVirtualizedList extends BaseVirtualizedList {
  _loomInCellsUpdate: boolean = false;

  constructor(props: any) {
    super(props);
    // $FlowFixMe[prop-missing]
    const original = this._updateCellsToRender;
    // $FlowFixMe[cannot-write]
    this._updateCellsToRender = (onUpdateComplete?: () => void) => {
      this._loomInCellsUpdate = true;
      try {
        original(onUpdateComplete);
      } finally {
        this._loomInCellsUpdate = false;
      }
    };
  }

  // $FlowFixMe[incompatible-type]
  setState(partialState: any, callback?: () => void): void {
    // An app can opt out with `global.__loomListTransitions = false`.
    // $FlowFixMe[prop-missing]
    if (this._loomInCellsUpdate && global.__loomListTransitions !== false) {
      startTransition(() => {
        super.setState(partialState, callback);
      });
      return;
    }
    super.setState(partialState, callback);
  }
}

const LoomVirtualizedListComponent: typeof BaseVirtualizedList =
  isJavaScriptOnMainThread()
    ? // $FlowFixMe[incompatible-type]
      LoomVirtualizedList
    : BaseVirtualizedList;

export default LoomVirtualizedListComponent;
