/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file in the root directory.
 */
#pragma once

#include <chrono>
#include <cstdint>

#include <react/renderer/runtimescheduler/LoomSettings.h>
#include <react/renderer/uimanager/LoomPlatformTraits.h>

namespace facebook::react {
// Runtime opt-in for profiling (`Loom.profileWorker`, see LoomSettings.h), without changing Release build flags.
struct LoomWorkerProfile {
  using Clock = std::chrono::steady_clock;
  struct Metrics {
    double prepare{0}, layout{0}, diff{0}, effects{0}, text{0};
    unsigned textCalls{0};
  };
  static bool enabled() {
    return LoomSettings::enabled(LoomSettingId::ProfileWorker);
  }
  static double ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
  }
  static int64_t ticks(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               time.time_since_epoch())
        .count();
  }
  static Metrics& metrics();
  struct TextScope {
    Clock::time_point start{};
    TextScope() {
      if (enabled())
        start = Clock::now();
    }
    ~TextScope() {
      if (enabled()) {
        metrics().text += ms(start, Clock::now());
        ++metrics().textCalls;
      }
    }
  };
};

// Per thread. A namespace-scope inline variable, not a function-local static: see
// LoomPlatformTraits.h (per-module copies under hidden inlines and prelinking).
inline thread_local LoomWorkerProfile::Metrics gLoomWorkerProfileMetrics{};

inline LoomWorkerProfile::Metrics& LoomWorkerProfile::metrics() {
  return gLoomWorkerProfileMetrics;
}
} // namespace facebook::react
