/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.modules.core

import com.facebook.proguard.annotations.DoNotStrip

/**
 * Loom: reports Choreographer frame start times to the native JS scheduler (implemented
 * in JJSTimerExecutor.cpp), which uses them to estimate the next vsync.
 */
@DoNotStrip
internal object ReactFrameClock {
  @JvmStatic external fun onFrame(frameTimeNanos: Long)

  private val handler by lazy { android.os.Handler(android.os.Looper.getMainLooper()) }

  /** Runs [runnable] on the UI thread after [delayMs]; used to re-post work behind a frame. */
  @JvmStatic
  fun postDelayed(runnable: Runnable, delayMs: Long) {
    handler.postDelayed(runnable, delayMs)
  }
}
