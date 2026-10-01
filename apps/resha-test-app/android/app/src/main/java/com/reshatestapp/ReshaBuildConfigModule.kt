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
    switches.putBoolean("rnUiThread", com.facebook.react.BuildConfig.RN_UI_THREAD)
    switches.putBoolean("uiThreadRenderer", com.facebook.react.BuildConfig.UI_THREAD_RENDERER)
    switches.putBoolean(
        "uiThreadRendererWorker",
        com.facebook.react.BuildConfig.UI_THREAD_RENDERER_WORKER,
    )
    switches.putBoolean("profile", com.facebook.react.BuildConfig.RN_PROFILE_UI_THREAD_RENDERER)
    promise.resolve(switches)
  }

  companion object {
    const val NAME = "ReshaBuildConfig"
  }
}