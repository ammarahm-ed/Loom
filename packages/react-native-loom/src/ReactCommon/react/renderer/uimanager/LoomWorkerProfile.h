/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file in the root directory.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#if defined(__APPLE__)
#include <os/log.h>
#include <pthread.h>
#elif defined(__ANDROID__)
#include <android/log.h>
#endif

namespace facebook::react {
// Runtime opt-in for simulator profiling, without changing Release build flags.
struct LoomWorkerProfile {
  using Clock = std::chrono::steady_clock;
  struct Metrics {
    double prepare{0}, layout{0}, diff{0}, effects{0}, text{0};
    unsigned textCalls{0};
  };
  static bool enabled() {
#if defined(__APPLE__)
    static const bool value = [] {
      auto* flag = std::getenv("LOOM_PROFILE_WORKER");
      return flag && flag[0] == '1';
    }();
    return value;
#elif defined(__ANDROID__) && LOOM_PROFILE
    return true;
#else
    return false;
#endif
  }
  static double ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
  }
  static int64_t ticks(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               time.time_since_epoch())
        .count();
  }
  static Metrics& metrics() {
    static thread_local Metrics value;
    return value;
  }
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
} // namespace facebook::react
