package com.loomexample

import com.facebook.react.BaseReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.model.ReactModuleInfoProvider

/** Loads the native library, whose JNI_OnLoad registers the C++ TurboModule. It has no Java module. */
class LoomExamplePackage : BaseReactPackage() {
  init {
    System.loadLibrary("loomexample")
  }

  override fun getModule(name: String, reactContext: ReactApplicationContext): NativeModule? = null

  override fun getReactModuleInfoProvider() = ReactModuleInfoProvider { emptyMap() }
}
