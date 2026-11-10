package com.reshatestapp

import android.os.Bundle
import android.view.MotionEvent
import com.facebook.react.ReactActivity
import com.facebook.react.ReactActivityDelegate
import com.facebook.react.defaults.DefaultReactActivityDelegate

class MainActivity : ReactActivity() {

  /**
   * Returns the name of the main component registered from JavaScript. This is used to schedule
   * rendering of the component.
   */
  override fun getMainComponentName(): String = "ReshaTestApp"

  /**
   * Returns the instance of the [ReactActivityDelegate].
   *
   * The `scenario` intent extra (`adb shell am start ... --es scenario scroll`) is passed to JS as
   * an initial prop, which makes App render that benchmark instead of the smoke test.
   */
  override fun createReactActivityDelegate(): ReactActivityDelegate =
      object : DefaultReactActivityDelegate(this, mainComponentName) {
        override fun getLaunchOptions(): Bundle? =
            intent.getStringExtra("scenario")?.let { scenario ->
              Bundle().apply { putString("scenario", scenario) }
            }
      }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    if (intent.hasExtra("scenario")) {
      BenchProbe.attach(this)
    }
  }

  override fun dispatchTouchEvent(event: MotionEvent): Boolean {
    if (intent.hasExtra("scenario")) {
      BenchProbe.onTouch(event)
    }
    return super.dispatchTouchEvent(event)
  }
}
