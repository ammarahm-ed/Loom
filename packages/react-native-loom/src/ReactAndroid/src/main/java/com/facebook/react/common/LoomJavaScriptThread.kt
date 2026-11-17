/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.react.common

import android.content.Context
import android.content.pm.ApplicationInfo
import com.facebook.proguard.annotations.DoNotStrip

/**
 * Loom: where JavaScript runs, a startup setting rather than a build one.
 *
 * `loom.js=main|background` in the app's gradle.properties becomes the `loom_js_on_main` bool
 * resource of the app (`loom.settings.gradle`); [initialize] reads it when the React instance is
 * created, before any JS or native-modules thread exists, hands it to the native layer
 * (`LoomJavaScriptThread` in the runtime scheduler) and keeps it here for the Java half. It is
 * fixed for the life of the process: a later instance with another value is a programming error.
 *
 * Without the resource (an app that does not use the Loom gradle integration) JavaScript stays on
 * its own thread, as in upstream React Native.
 */
@DoNotStrip
public object LoomJavaScriptThread {
  @Volatile private var fixed: Boolean? = null

  /** True when JavaScript runs on the UI thread. False until [initialize] has run. */
  @JvmStatic
  public fun isOnUIThread(): Boolean = fixed ?: false

  /** Reads the app's setting and fixes it. Call before the JS thread and the scheduler exist. */
  @JvmStatic
  @Synchronized
  public fun initialize(context: Context) {
    if (fixed != null) {
      return
    }
    val id = context.resources.getIdentifier("loom_js_on_main", "bool", context.packageName)
    var onUIThread = id != 0 && context.resources.getBoolean(id)
    // Development override so one installed binary can run both modes, for debuggable apps only:
    // `adb shell setprop debug.loom.js main|background`, then restart the app.
    if ((context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
      val override =
          try {
            Class.forName("android.os.SystemProperties")
                .getMethod("get", String::class.java, String::class.java)
                .invoke(null, "debug.loom.js", "") as String
          } catch (e: Exception) {
            ""
          }
      if (override == "main") onUIThread = true else if (override == "background") onUIThread = false
    }
    fixed = onUIThread
    nativeSet(onUIThread)
  }

  /** Native half of the setting (JJSTimerExecutor.cpp). */
  @JvmStatic private external fun nativeSet(onUIThread: Boolean)
}
