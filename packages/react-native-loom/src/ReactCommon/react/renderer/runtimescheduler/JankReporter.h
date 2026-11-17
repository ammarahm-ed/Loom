/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Loom: main-thread jank reporter. Debug builds only: with
// JavaScript on the UI thread (main mode) a slow task drops frames, and in
// development the app author needs to know what the slow task was. This header
// compiles to nothing in release builds (NDEBUG), so release carries no
// watchdog thread, no per-task bookkeeping and no JS bindings. The platform
// build turns it on with `RN_JANK_REPORTER=1`.
#if !defined(RN_JANK_REPORTER) || defined(NDEBUG)
#undef RN_JANK_REPORTER
#define RN_JANK_REPORTER 0
#endif

#if RN_JANK_REPORTER

#include <jsi/jsi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "LoomJavaScriptThread.h"
#include "LoomSettings.h"

namespace facebook::react::jank {

/** What kind of work a main-thread task was running. */
enum class Cause : uint8_t { Other, Event, Timer, AnimationFrame, Commit };

/** Per-task time split, in milliseconds. */
struct TaskTimes {
  double taskMs{0};
  double microtaskMs{0};
  double renderingMs{0};
  double totalMs() const {
    return taskMs + microtaskMs + renderingMs;
  }
};

/** Engine hooks for sampling the JS stack of an over-budget task. */
struct JsSamplingHooks {
  std::function<void(double hz)> enable;
  std::function<void()> disable;
  /** Drains the samples taken since `enable` into a few readable lines. */
  std::function<std::string()> summarize;
};

class Reporter {
 public:
  static Reporter& instance();

  /** Milliseconds a main-thread task may take before it is reported (`Loom.jankBudgetMs`). */
  int budgetMs() const noexcept {
    return LoomSettings::get(LoomSettingId::JankBudgetMs);
  }
  /** Main mode only: with JavaScript on its own thread a slow task does not drop frames. */
  bool enabled() const noexcept {
    return loomJavaScriptOnUIThread() &&
        LoomSettings::enabled(LoomSettingId::JankReporter);
  }

  /**
   * Hot path. Two relaxed-order atomic stores and one clock read. No
   * allocation, no syscall (the clock is the vDSO), no lock.
   */
  void beginTask() noexcept;
  /** Ends the task; the slow path runs only if it was over budget. */
  void endTask(const TaskTimes& times);

  /** Names the work the current task is doing. First cause in a task wins. */
  static void setCause(Cause cause, const char* label) noexcept;
  /** Counts a native commit made during the task and how long it took. */
  static void addCommit(double ms) noexcept;
  /** Records where the callback that is running was scheduled (debug JS). */
  void noteScheduleSite(std::string site, double ms);

  void setJsSamplingHooks(JsSamplingHooks hooks);
  /** Returns the formatted Java/native main-thread stack, or empty. */
  void setNativeStackSampler(std::function<std::string()> sampler);

  /**
   * Called at the start of a later task, on the JS thread: logs a pending
   * report through console.warn. Never called mid-task.
   */
  void flushPending(jsi::Runtime& runtime);

  /** Installs the `__loomJank*` host functions the dev JS module uses. */
  void installJsBindings(jsi::Runtime& runtime);

  /** Called by the Java side: queue a message for the next flush. */
  void enqueueMessage(std::string message);

 private:
  Reporter();
  struct Impl;
  Impl* impl_;
};

} // namespace facebook::react::jank

#endif // RN_JANK_REPORTER
