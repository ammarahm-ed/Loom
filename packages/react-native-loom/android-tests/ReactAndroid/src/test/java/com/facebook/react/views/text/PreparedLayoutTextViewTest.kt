/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.views.text

import android.graphics.Paint
import android.text.Layout
import android.text.SpannableString
import android.text.TextPaint
import android.text.BoringLayout
import android.view.accessibility.AccessibilityNodeInfo
import org.assertj.core.api.Assertions.assertThat
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment

@RunWith(RobolectricTestRunner::class)
class PreparedLayoutTextViewTest {

  @Test
  fun exposesPreparedTextThroughAccessibilityNode() {
    val text = SpannableString("prepared text remains accessible")
    val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 20f }
    val metrics = BoringLayout.isBoring(text, paint)!!
    val layout =
        BoringLayout.make(
            text,
            paint,
            metrics.width,
            Layout.Alignment.ALIGN_NORMAL,
            1f,
            0f,
            metrics,
            true,
        )
    val view = PreparedLayoutTextView(RuntimeEnvironment.getApplication())
    view.preparedLayout = PreparedLayout(layout, 0, 0f, intArrayOf(), 0, 0)
    val node = AccessibilityNodeInfo.obtain()

    view.onInitializeAccessibilityNodeInfo(node)

    assertThat(node.text.toString()).isEqualTo(text.toString())
    node.recycle()
  }

  @Test
  fun routesSpanlessPreparedTextToItsOwningReactTag() {
    val text = SpannableString("plain text")
    val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 20f }
    val metrics = BoringLayout.isBoring(text, paint)!!
    val layout =
        BoringLayout.make(
            text,
            paint,
            metrics.width,
            Layout.Alignment.ALIGN_NORMAL,
            1f,
            0f,
            metrics,
            true,
        )
    val view = PreparedLayoutTextView(RuntimeEnvironment.getApplication()).apply {
      id = 4207
      preparedLayout = PreparedLayout(layout, 0, 0f, intArrayOf(), 0, 0)
    }

    assertThat(view.reactTagForTouch(1f, 1f)).isEqualTo(4207)
  }
}
