package com.loomtestapp

import android.app.Activity
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.ViewTreeObserver
import com.facebook.react.uimanager.util.ReactFindViewUtil

/**
 * Native timing hooks for the benchmark scenarios (see Bench.tsx and bench/run.js).
 *
 * Everything here is measured on the UI thread with one clock, SystemClock.uptimeMillis(), which is
 * also the time base of MotionEvent.getEventTime(). That keeps the numbers comparable between stock
 * React Native, where JS runs on its own thread, and the UI thread builds, where it does not: the
 * timestamps are taken where pixels are produced, not where JS thinks it committed.
 *
 *  - `ready`: the first pre-draw in which the view with nativeID `bench-ready` exists, reported as
 *    milliseconds since the process was forked (time to first React content on screen).
 *  - `down`: the event time of every ACTION_DOWN the activity receives.
 *  - `probe`: the first pre-draw after the view with nativeID `latency-probe` changed alpha.
 */
object BenchProbe {
  private const val TAG = "LoomBench"

  private var readyReported = false
  private var lastProbeAlpha = Float.NaN

  fun attach(activity: Activity) {
    maybeProfileJs(activity)
    val root = activity.window.decorView
    root.viewTreeObserver.addOnPreDrawListener(
        object : ViewTreeObserver.OnPreDrawListener {
          override fun onPreDraw(): Boolean {
            onPreDraw(root)
            return true
          }
        })
  }

  /**
   * `--ei jsprofile <ms>` (with optional `--ei jsprofileDelay <ms>`, default 2000): samples the
   * JavaScript stack with Hermes's sampling profiler for that window and writes a Chrome trace to
   * the app's external files dir (`/sdcard/Android/data/<package>/files/jsprofile.json`), which
   * `adb pull` can read on a release build. Bench-only; does nothing without the extra.
   */
  private fun maybeProfileJs(activity: Activity) {
    val durationMs = activity.intent.getIntExtra("jsprofile", 0)
    if (durationMs <= 0) return
    val delayMs = activity.intent.getIntExtra("jsprofileDelay", 2000).toLong()
    val out = java.io.File(activity.getExternalFilesDir(null), "jsprofile.json")
    val handler = android.os.Handler(android.os.Looper.getMainLooper())
    handler.postDelayed(
        {
          com.facebook.hermes.instrumentation.HermesSamplingProfiler.enable()
          Log.i(TAG, "jsprofile start")
          handler.postDelayed(
              {
                com.facebook.hermes.instrumentation.HermesSamplingProfiler.disable()
                com.facebook.hermes.instrumentation.HermesSamplingProfiler.dumpSampledTraceToFile(
                    out.absolutePath)
                Log.i(TAG, "jsprofile written ${out.absolutePath}")
              },
              durationMs.toLong())
        },
        delayMs)
  }

  fun onTouch(event: MotionEvent) {
    if (event.actionMasked == MotionEvent.ACTION_DOWN) {
      Log.i(TAG, "down t=${event.eventTime}")
    }
  }

  private fun onPreDraw(root: View) {
    val now = SystemClock.uptimeMillis()
    if (!readyReported && ReactFindViewUtil.findView(root, "bench-ready") != null) {
      readyReported = true
      Log.i(TAG, "ready t=$now sinceStart=${now - Process.getStartUptimeMillis()}")
    }
    val probe = ReactFindViewUtil.findView(root, "latency-probe") ?: return
    val alpha = probe.alpha
    if (alpha != lastProbeAlpha) {
      if (!lastProbeAlpha.isNaN()) {
        Log.i(TAG, "probe alpha=$alpha t=$now")
      }
      lastProbeAlpha = alpha
    }
  }
}
