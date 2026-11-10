package com.reshatestapp

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.module.annotations.ReactModule

/**
 * Reports the Resha switches this binary was actually compiled with, so the smoke screen can show
 * the configuration that is running instead of guessing from the JS side.
 *
 * The values come from ReactAndroid's own `BuildConfig`, which is generated from the same Gradle
 * properties that feed the CMake options, so this is the compiled-in truth and not a re-derivation.
 */
@ReactModule(name = ReshaBuildConfigModule.NAME)
class ReshaBuildConfigModule(reactContext: ReactApplicationContext) :
    ReactContextBaseJavaModule(reactContext) {

  override fun getName(): String = NAME

  @ReactMethod
  fun getSwitches(promise: Promise) {
    val switches = Arguments.createMap()
    switches.putBoolean("rnUiThread", flag("RN_UI_THREAD"))
    switches.putBoolean("uiThreadRenderer", flag("UI_THREAD_RENDERER"))
    switches.putBoolean("uiThreadRendererWorker", flag("UI_THREAD_RENDERER_WORKER"))
    switches.putBoolean("profile", flag("RN_PROFILE_UI_THREAD_RENDERER"))
    // Stock react-android has none of these fields: that is how the benchmark's baseline build,
    // which uses the prebuilt artifacts from Maven, tells itself apart from an all-off Resha build.
    switches.putBoolean("stock", !hasFlag("RN_UI_THREAD"))
    promise.resolve(switches)
  }

  // Read reflectively so the same app source compiles against stock React Native too.
  private fun field(name: String) =
      runCatching { com.facebook.react.BuildConfig::class.java.getField(name) }.getOrNull()

  private fun hasFlag(name: String) = field(name) != null

  private fun flag(name: String) = field(name)?.getBoolean(null) ?: false

  companion object {
    const val NAME = "ReshaBuildConfig"
  }
}