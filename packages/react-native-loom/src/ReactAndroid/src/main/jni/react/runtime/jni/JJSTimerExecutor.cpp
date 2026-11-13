/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JJSTimerExecutor.h"

#include <fbjni/fbjni.h>
#include <fbjni/NativeRunnable.h>
#include <jni.h>
#include <cstdlib>
#include <react/renderer/runtimescheduler/JankReporter.h>
#include <react/renderer/runtimescheduler/RuntimeSchedulerFrameCallbackScope.h>

namespace facebook::react {

void JJSTimerExecutor::initHybrid(
    jni::alias_ref<JJSTimerExecutor::jhybridobject> jobj) {
  setCxxInstance(jobj);
}

void JJSTimerExecutor::setTimerManager(
    std::weak_ptr<TimerManager> timerManager) {
  timerManager_ = timerManager;
}

void JJSTimerExecutor::callTimers(WritableNativeArray* timerIDs) {
  if (auto timerManager = timerManager_.lock()) {
    const auto ids = timerIDs->consume();
#if LOOM_JS_ON_MAIN
    // Loom: called from Choreographer's animation phase on the UI
    // thread, which is also the JS thread. requestAnimationFrame callbacks, and
    // whatever React renders in response, run before this frame's traversal
    // instead of after it (a posted message queues behind the traversal's sync
    // barrier and the commit would be drawn a frame late); see
    // FrameCallbackScope. Plain timers keep the posted path: they do not ask to
    // be drawn in this frame, and running their (possibly long) JS ahead of the
    // draw would only delay it.
    bool hasAnimationFrame = false;
    for (const auto& timerID : ids) {
      const auto handle = (uint32_t)timerID.asInt();
      if (timerManager->isAnimationFrameTimer(handle)) {
        hasAnimationFrame = true;
      } else {
        timerManager->callTimer(handle);
      }
    }
    if (hasAnimationFrame) {
      // The scope's exit runs the event loop and applies the frame's commits.
      FrameCallbackScope frameScope;
      for (const auto& timerID : ids) {
        const auto handle = (uint32_t)timerID.asInt();
        if (timerManager->isAnimationFrameTimer(handle)) {
          timerManager->callTimer(handle);
        }
      }
    }
#else
    for (const auto& timerID : ids) {
      timerManager->callTimer((uint32_t)timerID.asInt());
    }
#endif
  }
}

namespace {
#if LOOM_JS_ON_MAIN
void reactFrameClockOnFrame(jni::alias_ref<jclass>, jlong frameTimeNanos) {
  FrameClock::onFrame(frameTimeNanos);
}
#endif
} // namespace

void JJSTimerExecutor::registerNatives() {
#if LOOM_JS_ON_MAIN
  jni::registerNatives(
      "com/facebook/react/modules/core/ReactFrameClock",
      {makeNativeMethod("onFrame", reactFrameClockOnFrame)});
  LoomRuntimeHooks hooks;
  hooks.postDelayed = [](std::function<void()> task, int64_t delayNanos) {
    static auto cls =
        jni::findClassStatic("com/facebook/react/modules/core/ReactFrameClock");
    static auto post = cls->getStaticMethod<void(jni::alias_ref<jni::JRunnable::javaobject>, jlong)>(
        "postDelayed");
    post(cls, jni::JNativeRunnable::newObjectCxxArgs(std::move(task)).get(), delayNanos / 1000000);
  };
  LoomRuntimeHooks::install(hooks);
#endif
#if RN_JANK_REPORTER
  // Loom (debug): the watchdog samples the main thread's Java stack
  // through this while a JS task is over budget.
  jank::Reporter::instance().setNativeStackSampler([]() -> std::string {
    try {
      jni::ThreadScope scope;
      static auto cls = jni::findClassStatic(
          "com/facebook/react/common/JankReporterSupport");
      static auto method = cls->getStaticMethod<jstring()>("mainThreadStack");
      return method(cls)->toStdString();
    } catch (...) {
      return {};
    }
  });
#endif
  registerHybrid({
      makeNativeMethod("callTimers", JJSTimerExecutor::callTimers),
      makeNativeMethod("initHybrid", JJSTimerExecutor::initHybrid),
  });
}

} // namespace facebook::react
