package com.loomcompatapp

import android.os.Bundle
import com.facebook.react.ReactActivity
import com.facebook.react.ReactActivityDelegate
import com.facebook.react.defaults.DefaultReactActivityDelegate

class MainActivity : ReactActivity() {

  /**
   * Returns the name of the main component registered from JavaScript. This is used to schedule
   * rendering of the component.
   */
  override fun getMainComponentName(): String = "LoomCompatApp"

  /**
   * `adb shell am start ... --es scenario <lib>|all` is passed to JS as an initial prop, which
   * makes App.tsx render that library's self-check screen.
   */
  override fun createReactActivityDelegate(): ReactActivityDelegate =
      object : DefaultReactActivityDelegate(this, mainComponentName) {
        override fun getLaunchOptions(): Bundle? =
            intent.getStringExtra("scenario")?.let { scenario ->
              Bundle().apply { putString("scenario", scenario) }
            }
      }
}
