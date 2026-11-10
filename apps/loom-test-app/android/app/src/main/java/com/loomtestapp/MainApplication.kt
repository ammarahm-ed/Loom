package com.loomtestapp

import android.app.Application
import android.util.Log
import com.facebook.react.PackageList
import com.facebook.react.ReactApplication
import com.facebook.react.ReactHost
import com.facebook.react.ReactNativeApplicationEntryPoint.loadReactNative
import com.facebook.react.defaults.DefaultReactHost.getDefaultReactHost
import com.facebook.react.internal.featureflags.ReactNativeFeatureFlags
import com.facebook.react.internal.featureflags.ReactNativeNewArchitectureFeatureFlagsDefaults

class MainApplication : Application(), ReactApplication {

  override val reactHost: ReactHost by lazy {
    getDefaultReactHost(
      context = applicationContext,
      packageList =
        PackageList(this).packages.apply {
          // Reports the Loom switches this binary was compiled with, so the smoke screen can show
          // which configuration is actually running.
          add(LoomBuildConfigPackage())
        },
    )
  }

  override fun onCreate() {
    super.onCreate()
    loadReactNative(this)
    if (BuildConfig.BENCH_TUNED_FLAGS) {
      // The benchmark's "tuned stock" baseline: stock React Native with the prepared text layout,
      // text measurement and view recycling flags on, which the Loom renderer forces on itself.
      // Upstream turns the text ones on by default in 0.89. Forced after loadReactNative() because
      // that already installed the defaults; the log shows any flag read before this point.
      val accessed =
          ReactNativeFeatureFlags.dangerouslyForceOverride(
              object : ReactNativeNewArchitectureFeatureFlagsDefaults() {
                override fun enablePreparedTextLayout(): Boolean = true

                override fun enableAndroidTextMeasurementOptimizations(): Boolean = true

                override fun enableViewRecycling(): Boolean = true

                override fun preparedTextCacheSize(): Double = 1000.0
              })
      Log.i("LoomBench", "tuned flags on; flags read before the override: $accessed")
    }
  }
}
