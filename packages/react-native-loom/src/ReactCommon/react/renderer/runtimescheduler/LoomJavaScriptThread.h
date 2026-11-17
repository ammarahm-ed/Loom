/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

namespace facebook::react {

/**
 * Where React Native's JavaScript runtime runs: on the thread that owns native
 * views ("main"), or on a thread of its own ("background").
 *
 * The mode is a startup decision, fixed for the life of the process, and the one
 * source of truth for every Loom layer (runtime scheduler, renderer binding,
 * worker, event manager, platform glue). A host sets it once, before the JS
 * thread or the runtime scheduler exist:
 *
 *   - iOS: a generated constant in the app's `LoomRuntimeConfig` pod (Podfile
 *     `loom.js`), applied before `main`.
 *   - Android: the app's `loom.js` gradle property, read by `ReactInstance`.
 *   - Desktop and headless hosts: `LoomJavaScriptThread::set` from their own config.
 *
 * Reading is a relaxed atomic load. Setting a different value after the first
 * read is a programming error (debug assert; ignored in release so that the
 * process keeps one consistent mode). With nothing set the mode is "main".
 */
class LoomJavaScriptThread final {
 public:
  /** True when JavaScript runs on the UI thread. */
  static bool onUIThread() noexcept {
    const auto state = state_.load(std::memory_order_relaxed);
    if (state == kUnset) [[unlikely]] {
      return resolveDefault();
    }
    if (!read_.load(std::memory_order_relaxed)) [[unlikely]] {
      read_.store(true, std::memory_order_relaxed);
    }
    return state == kMain;
  }

  /**
   * Fixes the mode. Call once, before the JS thread and the runtime scheduler
   * exist. Repeating the same value is harmless.
   */
  static void set(bool javaScriptOnUIThread) noexcept {
    const auto wanted = javaScriptOnUIThread ? kMain : kBackground;
    const auto previous = state_.load(std::memory_order_relaxed);
    if (read_.load(std::memory_order_relaxed) && previous != wanted) {
      assert(false && "Loom: the JavaScript thread mode was set after it was read");
      return;
    }
    state_.store(wanted, std::memory_order_relaxed);
  }

  /**
   * For a platform whose default differs from "main": fixes the mode only if
   * nothing set it yet (the platform config usually has, before this runs).
   */
  static void setDefault(bool javaScriptOnUIThread) noexcept {
    int8_t expected = kUnset;
    state_.compare_exchange_strong(
        expected, javaScriptOnUIThread ? kMain : kBackground, std::memory_order_relaxed);
  }

#if defined(LOOM_TEST_HOOKS) && LOOM_TEST_HOOKS
  /** Tests only: forgets everything, so one binary can run both modes. */
  static void resetForTesting(bool javaScriptOnUIThread) noexcept {
    read_.store(false, std::memory_order_relaxed);
    state_.store(javaScriptOnUIThread ? kMain : kBackground, std::memory_order_relaxed);
  }
#endif

 private:
  static constexpr int8_t kUnset = 0;
  static constexpr int8_t kMain = 1;
  static constexpr int8_t kBackground = 2;

  static bool resolveDefault() noexcept {
    int8_t expected = kUnset;
    state_.compare_exchange_strong(expected, kMain, std::memory_order_relaxed);
    read_.store(true, std::memory_order_relaxed);
    return state_.load(std::memory_order_relaxed) == kMain;
  }

  // Process-wide state lives in `static inline` data members, never in a function-local
  // `static` of an inline function (see LoomPlatformTraits): one definition across targets.
  static inline std::atomic<int8_t> state_{kUnset};
  static inline std::atomic<bool> read_{false};
};

/** Shorthand for `LoomJavaScriptThread::onUIThread()`. */
inline bool loomJavaScriptOnUIThread() noexcept {
  return LoomJavaScriptThread::onUIThread();
}

#if defined(LOOM_TEST_HOOKS) && LOOM_TEST_HOOKS
/**
 * Tests only: counts what each mode's paths do, so a test can assert that a mode
 * never touches the other's machinery.
 */
struct LoomModeCounters {
  /** The pre-traversal frame drain ran (`LoomWorker::synchronizeFor`). Main only. */
  static inline std::atomic<uint32_t> frameDrains{0};
  /** The after-frame hook was installed or ran. Main only. */
  static inline std::atomic<uint32_t> afterFrameHookInstalls{0};
  static inline std::atomic<uint32_t> afterFrameHookRuns{0};
  /** A runtime scheduler took a main-thread-only path (tick runtime scope, slicing). Main only. */
  static inline std::atomic<uint32_t> schedulerMainPaths{0};
  /** A non-UI thread blocked until the UI thread applied results (`awaitApplied`). Background only. */
  static inline std::atomic<uint32_t> jsToUIWaits{0};
  /** Work was posted to the UI thread to refresh text capability. Background only. */
  static inline std::atomic<uint32_t> uiCapabilityPosts{0};

  static void reset() {
    frameDrains = 0;
    afterFrameHookInstalls = 0;
    afterFrameHookRuns = 0;
    schedulerMainPaths = 0;
    jsToUIWaits = 0;
    uiCapabilityPosts = 0;
  }
};
#define LOOM_MODE_COUNT(name) \
  (::facebook::react::LoomModeCounters::name.fetch_add(1, std::memory_order_relaxed))
#else
#define LOOM_MODE_COUNT(name) ((void)0)
#endif

} // namespace facebook::react
