package com.loomtestapp

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.WritableMap
import com.facebook.react.module.annotations.ReactModule
import android.os.Debug

/**
 * Reports the Loom switches this binary was actually compiled with, so the smoke screen can show
 * the configuration that is running instead of guessing from the JS side.
 *
 * The values come from ReactAndroid's own `BuildConfig`, which is generated from the same Gradle
 * properties that feed the CMake options, so this is the compiled-in truth and not a re-derivation.
 */
@ReactModule(name = LoomBuildConfigModule.NAME)
class LoomBuildConfigModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

  override fun getName(): String = NAME

  @ReactMethod
  fun getSwitches(promise: Promise) {
    val switches = Arguments.createMap()
    switches.putBoolean("rnUiThread", jsOnUiThread())
    switches.putBoolean("loom", flag("LOOM_RENDERER"))
    switches.putBoolean("loomWorker", flag("LOOM_WORKER"))
    switches.putBoolean("profile", flag("LOOM_PROFILE"))
    // Stock react-android has none of these fields: that is how the benchmark's baseline build,
    // which uses the prebuilt artifacts from Maven, tells itself apart from an all-off Loom build.
    switches.putBoolean("stock", !hasFlag("LOOM_RENDERER"))
    promise.resolve(switches)
  }

  // Memory for the `memory` scenario: total PSS, native heap allocated and Java heap in use, in KB.
  @ReactMethod(isBlockingSynchronousMethod = true)
  fun memSample(): WritableMap {
    val info = Debug.MemoryInfo()
    Debug.getMemoryInfo(info)
    val map = Arguments.createMap()
    map.putDouble("footprintKb", info.totalPss.toDouble())
    map.putDouble("heapKb", (Debug.getNativeHeapAllocatedSize() / 1024).toDouble())
    map.putDouble("nativeKb", (Debug.getNativeHeapAllocatedSize() / 1024).toDouble())
    val rt = Runtime.getRuntime()
    map.putDouble("javaKb", ((rt.totalMemory() - rt.freeMemory()) / 1024).toDouble())
    return map
  }

  // Where JavaScript runs is the app's startup setting (`loom.js`), read by Loom's ReactInstance; the
  // class is absent from stock React Native.
  private fun jsOnUiThread(): Boolean =
      runCatching {
            Class.forName("com.facebook.react.common.LoomJavaScriptThread")
                .getMethod("isOnUIThread")
                .invoke(null) as Boolean
          }
          .getOrDefault(false)

  /** Same contract as the iOS module: a line in logcat, tag LoomBench. */
  @ReactMethod
  fun log(line: String) {
    android.util.Log.i("LoomBench", line)
  }

  /** The thread JavaScript is running on right now (a blocking sync method runs on the JS thread). */
  @ReactMethod(isBlockingSynchronousMethod = true)
  fun jsThread(): String =
      "main=" + (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) +
          " name=" + Thread.currentThread().name

  // Read reflectively so the same app source compiles against stock React Native too.
  private fun field(name: String) =
      runCatching { com.facebook.react.BuildConfig::class.java.getField(name) }.getOrNull()

  private fun hasFlag(name: String) = field(name) != null

  private fun flag(name: String) = field(name)?.getBoolean(null) ?: false

  companion object {
    const val NAME = "LoomBuildConfig"
  }
}