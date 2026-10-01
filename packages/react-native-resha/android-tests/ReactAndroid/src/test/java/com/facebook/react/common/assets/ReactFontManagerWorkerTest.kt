/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file in the root directory.
 */

package com.facebook.react.common.assets

import android.graphics.Typeface
import com.facebook.react.BuildConfig
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.assertj.core.api.Assertions.assertThat
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class ReactFontManagerWorkerTest {
  @Test
  fun measurementAndDrawingCanShareFontCachesDuringRegistration() {
    // The direct renderer measures text off the UI thread, so it synchronizes the shared font
    // caches on the UI thread as well. Without that build switch the caches stay lock-free, as
    // upstream, and concurrent access is not a supported contract.
    assumeTrue(BuildConfig.UI_THREAD_RENDERER)

    val manager = ReactFontManager()
    val start = CountDownLatch(1)
    val threads = Executors.newFixedThreadPool(4)
    try {
      val work = (0 until 4).map { thread ->
        threads.submit {
          start.await()
          repeat(200) { index ->
            val family = "worker-$thread-$index"
            manager.addCustomFont(family, Typeface.DEFAULT)
            manager.setTypeface(family, Typeface.NORMAL, Typeface.DEFAULT)
            assertThat(manager.getTypeface(family, Typeface.NORMAL, null)).isNotNull()
            assertThat(manager.customFontFamilies).contains(family)
            manager.getTypeface("sans-serif", Typeface.NORMAL, null)
          }
        }
      }
      start.countDown()
      work.forEach { it.get(20, TimeUnit.SECONDS) }
      assertThat(manager.customFontFamilies).hasSize(800)
    } finally {
      threads.shutdownNow()
    }
  }
}
