/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.common

import android.app.Activity
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.view.FrameMetrics
import android.view.Window
import com.facebook.proguard.annotations.DoNotStrip
import com.facebook.react.BuildConfig
import com.facebook.react.bridge.ReactContext
import java.lang.ref.WeakReference

/**
 * Loom: the Java half of the main-thread jank reporter.
 *
 * Debug builds only (every entry point returns immediately otherwise, and the callers are guarded
 * by [BuildConfig.DEBUG], so release carries neither the frame listener nor the sampler thread's
 * target). It does three things the C++ side cannot:
 *
 * - samples the main thread's Java stack for the watchdog, while a JS task is over budget;
 * - times the native view apply and reports it when it is over budget;
 * - watches per-frame phase timings (`FrameMetrics`) and reports slow frames by phase.
 *
 * Reports reach the developer through a `LoomJank` device event that `setUpJankReporter` turns
 * into `console.warn`. Emitting posts to the JS runtime, so the warning is delivered by a later task
 * and never from inside the slow one.
 */
@DoNotStrip
public object JankReporterSupport {
  private const val EVENT = "LoomJank"

  // Same default as the C++ side and the web's long-task threshold. A debug build runs unoptimized
  // JS and C++, so a lower bar would report work that is comfortably fast in release.
  private val budgetMs: Int by lazy { readIntProperty("debug.loom.jank_budget_ms", 50) }
  private val enabled: Boolean by lazy { readIntProperty("debug.loom.jank", 1) != 0 }

  private var context: WeakReference<ReactContext>? = null
  private var lastReportNanos = 0L
  private var suppressed = 0
  private var frameThread: HandlerThread? = null
  private var listenerWindow: WeakReference<Window>? = null
  private var listener: Window.OnFrameMetricsAvailableListener? = null

  private fun readIntProperty(name: String, fallback: Int): Int =
      try {
        val cls = Class.forName("android.os.SystemProperties")
        val value = cls.getMethod("get", String::class.java, String::class.java)
            .invoke(null, name, "") as String
        if (value.isEmpty()) fallback else value.toInt()
      } catch (t: Throwable) {
        fallback
      }

  @JvmStatic
  public fun isActive(): Boolean = BuildConfig.DEBUG && enabled

  @JvmStatic
  public fun budgetMs(): Int = budgetMs

  @JvmStatic
  public fun attach(reactContext: ReactContext) {
    if (!isActive()) return
    context = WeakReference(reactContext)
  }

  /** Starts watching frame phases on [activity]'s window (API 24+). */
  @JvmStatic
  public fun attachFrameMetrics(activity: Activity?) {
    if (!isActive() || activity == null) return
    val window = activity.window ?: return
    if (listenerWindow?.get() === window) return
    detachFrameMetrics()
    val thread = frameThread ?: HandlerThread("loom-jank-frames").also {
      it.start()
      frameThread = it
    }
    val l = Window.OnFrameMetricsAvailableListener { _, metrics, _ -> onFrame(metrics) }
    window.addOnFrameMetricsAvailableListener(l, Handler(thread.looper))
    listenerWindow = WeakReference(window)
    listener = l
  }

  @JvmStatic
  public fun detachFrameMetrics() {
    val window = listenerWindow?.get()
    val l = listener
    if (window != null && l != null) {
      try {
        window.removeOnFrameMetricsAvailableListener(l)
      } catch (t: Throwable) {}
    }
    listenerWindow = null
    listener = null
  }

  private fun onFrame(metrics: FrameMetrics) {
    if (metrics.getMetric(FrameMetrics.FIRST_DRAW_FRAME) == 1L) return
    val totalMs = metrics.getMetric(FrameMetrics.TOTAL_DURATION) / 1e6
    if (totalMs < budgetMs) return
    fun ms(id: Int) = metrics.getMetric(id) / 1e6
    val phases =
        listOf(
            "input handling" to ms(FrameMetrics.INPUT_HANDLING_DURATION),
            "animation callbacks (rAF, timers, JS)" to ms(FrameMetrics.ANIMATION_DURATION),
            "layout/measure" to ms(FrameMetrics.LAYOUT_MEASURE_DURATION),
            "draw" to ms(FrameMetrics.DRAW_DURATION),
            "sync/upload" to ms(FrameMetrics.SYNC_DURATION),
            "GPU command issue" to ms(FrameMetrics.COMMAND_ISSUE_DURATION),
            "buffer swap" to ms(FrameMetrics.SWAP_BUFFERS_DURATION),
            "delay before the frame (main thread busy)" to ms(FrameMetrics.UNKNOWN_DELAY_DURATION),
        )
    val worst = phases.maxByOrNull { it.second } ?: return
    val detail = phases.filter { it.second >= 1.0 }.joinToString(", ") { "%s %.0f ms".format(it.first, it.second) }
    report(
        "[Loom] A frame took %.0f ms (debug build; release is usually several times faster). Longest phase: %s (%.0f ms). Phases: %s. Frames this long drop frames in release too if the work is anywhere near this slow; move it off the main thread."
            .format(totalMs, worst.first, worst.second, detail))
  }

  /** Called after the native view apply with how long it took and how big the batch was. */
  @JvmStatic
  public fun noteApply(elapsedNanos: Long, instructions: Int) {
    val ms = elapsedNanos / 1e6
    if (ms < budgetMs) return
    report(
        "[Loom] Main thread blocked %.0f ms (debug build; release is usually several times faster) applying a native view update batch of %d instructions. Large updates drop frames; render less per update or virtualize the list."
            .format(ms, instructions))
  }

  private fun report(message: String) {
    val now = System.nanoTime()
    var text = message
    synchronized(this) {
      if (lastReportNanos != 0L && now - lastReportNanos < 1_000_000_000L) {
        suppressed++
        return
      }
      lastReportNanos = now
      if (suppressed > 0) {
        text += "\n  (%d more slow frame/apply report(s) since the last one were not shown.)".format(suppressed)
        suppressed = 0
      }
    }
    val ctx = context?.get() ?: return
    if (ctx.hasActiveReactInstance()) {
      ctx.emitDeviceEvent(EVENT, text)
    }
  }

  private val boilerplate =
      arrayOf(
          "android.os.Looper",
          "android.os.Handler",
          "android.os.MessageQueue",
          "android.view.Choreographer",
          "android.app.ActivityThread",
          "com.android.internal.os",
          "java.lang.reflect",
          "dalvik.system",
          "java.lang.Thread",
          "com.facebook.jni",
          "com.facebook.react.bridge.queue.MessageQueueThreadHandler",
          "com.facebook.react.bridge.queue.MessageQueueThreadImpl",
          "com.facebook.react.bridge.GuardedRunnable",
          "com.facebook.react.bridge.ReactContext",
          "androidx.core.os",
      )

  /**
   * The main thread's top Java frames with the Looper/Handler/Choreographer plumbing removed, for
   * the native watchdog. Called from the watchdog thread; empty when the main thread is only in
   * the plumbing (i.e. it is inside JS or C++).
   */
  @JvmStatic
  @DoNotStrip
  public fun mainThreadStack(): String {
    if (!BuildConfig.DEBUG) return ""
    val trace = Looper.getMainLooper().thread.stackTrace
    val frames = ArrayList<String>(4)
    for (e in trace) {
      val cls = e.className
      if (boilerplate.any { cls.startsWith(it) }) continue
      frames.add("    " + e.toString())
      if (frames.size == 5) break
    }
    return frames.joinToString("\n")
  }
}
