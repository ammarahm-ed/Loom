/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.views.text

import android.text.Layout
import com.facebook.proguard.annotations.DoNotStrip

/**
 * Encapsulates an {android.text.Layout} along with any additional state needed to render or measure
 * it.
 */
@DoNotStrip
internal class PreparedLayout(
    val layout: Layout,
    val maximumNumberOfLines: Int,
    val verticalOffset: Float,
    val reactTags: IntArray,
    val textBreakStrategy: Int,
    val justificationMode: Int,
) {
  @DoNotStrip var profileSpannableBuildNanos: Long = 0
  @DoNotStrip var profilePaintSetupNanos: Long = 0
  @DoNotStrip var profileLayoutCreateNanos: Long = 0
  @DoNotStrip var profileIsBoringNanos: Long = 0
  @DoNotStrip var profileBoringMakeNanos: Long = 0
  @DoNotStrip var profileMeasurementResultNanos: Long = 0
  @DoNotStrip var profileEnabled: Boolean = false
  @DoNotStrip var profileLayoutKind: Int = 0
  @DoNotStrip var profileSimplePlainTextFastPath: Boolean = false
}
