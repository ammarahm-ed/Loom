/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace facebook::react {

/**
 * Loom's runtime settings: a fixed set of named values in atomics.
 *
 * Dependency free (no renderer, folly or platform headers) so the runtime
 * scheduler layer (the jank reporter) and the renderer read the same values.
 * A read is one relaxed load, the cost of the `static const` it replaced, and
 * takes no lock. A write changes what work that starts afterwards sees.
 *
 * Resolution, by the host at startup (see `configurePlatform`): the built-in
 * default, the app's native config, the legacy overrides (user defaults,
 * launch arguments, system properties, environment), and then JavaScript
 * writes through `write`. The hosts do the first three with `set`.
 */
enum class LoomSettingId : uint8_t {
  AsyncText,
  AsyncTextFirstDraw,
  AsyncTextResize,
  EarlyCreate,
  ParallelText,
  JankReporter,
  JankBudgetMs,
  ProfileWorker,
  /** Read-only build info. */
  JsOnMainThread,
  Worker,
  Count,
};

enum class LoomSettingType : uint8_t { Bool, Int };

enum LoomPlatform : uint8_t {
  /** No host configured (headless tests): every setting applies. */
  kLoomPlatformAny = 0,
  kLoomPlatformIOS = 1,
  kLoomPlatformAndroid = 2,
};

struct LoomSettingInfo {
  /** The name JavaScript uses; also the native config key (`loom.<name>` in an Android manifest, `<name>` in the iOS `Loom` dictionary). */
  const char* name;
  LoomSettingType type;
  /** Bit mask of `1 << LoomPlatform` for the platforms that act on it. */
  uint8_t platforms;
  bool readOnly;
  /** Default with no platform configured, and what a platform that does not own the setting reports. */
  int defaultValue;
  int iosDefault;
  int androidDefault;
  /** Smallest accepted value (Int settings). */
  int minimum;
  /** Legacy NSUserDefaults / launch-argument key (iOS), or null. */
  const char* iosKey;
  /** Legacy system property (Android), or null. */
  const char* androidProperty;
};

class LoomSettings {
 public:
  using Id = LoomSettingId;
  static constexpr size_t kCount = static_cast<size_t>(Id::Count);

  static constexpr uint8_t kBoth = (1 << kLoomPlatformIOS) | (1 << kLoomPlatformAndroid);
  static constexpr uint8_t kIOS = 1 << kLoomPlatformIOS;

  static constexpr LoomSettingInfo kTable[kCount] = {
      {"asyncText", LoomSettingType::Bool, kIOS, false, 1, 1, 1, 0, "LoomAsyncText", nullptr},
      {"asyncTextFirstDraw", LoomSettingType::Bool, kIOS, false, 1, 1, 1, 0, "LoomAsyncTextFirstDraw", nullptr},
      {"asyncTextResize", LoomSettingType::Bool, kIOS, false, 1, 1, 1, 0, "LoomAsyncTextResize", nullptr},
      {"earlyCreate", LoomSettingType::Bool, kBoth, false, 0, 1, 1, 0, "LoomEarlyCreate", "debug.loom.earlycreate"},
      {"parallelText", LoomSettingType::Bool, kBoth, false, 0, 1, 0, 0, "LoomParallelText", "debug.loom.paralleltext"},
      {"jankReporter", LoomSettingType::Bool, kBoth, false, 1, 1, 1, 0, "LoomJankReporter", "debug.loom.jank"},
      {"jankBudgetMs", LoomSettingType::Int, kBoth, false, 50, 50, 50, 1, "LoomJankBudgetMs", "debug.loom.jank_budget_ms"},
      {"profileWorker", LoomSettingType::Bool, kBoth, false, 0, 0, 0, 0, "LoomProfileWorker", nullptr},
      {"jsOnMainThread", LoomSettingType::Bool, kBoth, true, 1, 1, 1, 0, nullptr, nullptr},
      {"worker", LoomSettingType::Bool, kBoth, true, 0, 0, 0, 0, nullptr, nullptr},
  };

  enum class WriteResult : uint8_t {
    Ok,
    /** The setting is read-only. */
    ReadOnly,
    /** Another platform owns the setting; nothing was written. */
    NotApplicable,
  };

  /** Called after a successful `write`, on the writing thread. Null: none. */
  using ChangeListener = void (*)(Id id, int value);

  static const LoomSettingInfo& info(Id id) {
    return kTable[static_cast<size_t>(id)];
  }

  /** The setting named `name`, or `Id::Count` when there is none. */
  static Id find(const char* name) {
    for (size_t i = 0; i < kCount; ++i) {
      if (std::strcmp(kTable[i].name, name) == 0) {
        return static_cast<Id>(i);
      }
    }
    return Id::Count;
  }

  /** The hot-path read: one relaxed load. */
  static int get(Id id) {
    return values()[static_cast<size_t>(id)].load(std::memory_order_relaxed);
  }
  static bool enabled(Id id) {
    return get(id) != 0;
  }

  static uint8_t platform() {
    return platform_().load(std::memory_order_relaxed);
  }

  /** Whether the current platform acts on the setting. */
  static bool applies(Id id) {
    const auto current = platform();
    return current == kLoomPlatformAny || (info(id).platforms & (1 << current)) != 0;
  }

  /** What JavaScript sees: the stored value, or the default for a setting this platform does not own. */
  static int valueForJS(Id id) {
    return applies(id) ? get(id) : info(id).defaultValue;
  }

  /** The built-in default on the current platform. */
  static int builtInDefault(Id id) {
    const auto& entry = info(id);
    switch (platform()) {
      case kLoomPlatformIOS:
        return entry.iosDefault;
      case kLoomPlatformAndroid:
        return entry.androidDefault;
      default:
        return entry.defaultValue;
    }
  }

  /**
   * Host startup, first step: names the platform and resets every writable
   * setting to its built-in default. The host then applies native config and
   * legacy overrides with `set`. Call before any surface starts.
   */
  static void configurePlatform(uint8_t platform) {
    platform_().store(platform, std::memory_order_relaxed);
    for (size_t i = 0; i < kCount; ++i) {
      const auto id = static_cast<Id>(i);
      if (!info(id).readOnly) {
        store(id, builtInDefault(id));
      }
    }
  }

  /** Host-side write (config, overrides): ignores platform and read-only. */
  static void set(Id id, int value) {
    store(id, value);
  }

  /** The JavaScript write. */
  static WriteResult write(Id id, int value) {
    const auto& entry = info(id);
    if (entry.readOnly) {
      return WriteResult::ReadOnly;
    }
    if (!applies(id)) {
      return WriteResult::NotApplicable;
    }
    value = normalize(entry, value);
    store(id, value);
    const auto listener = listener_().load(std::memory_order_acquire);
    if (listener != nullptr) {
      listener(id, value);
    }
    return WriteResult::Ok;
  }

  /** Fills the read-only build info. */
  static void setBuildInfo(bool jsOnMainThread, bool worker) {
    store(Id::JsOnMainThread, jsOnMainThread ? 1 : 0);
    store(Id::Worker, worker ? 1 : 0);
  }

  static void setChangeListener(ChangeListener listener) {
    listener_().store(listener, std::memory_order_release);
  }

  /** Back to the unconfigured state (tests). */
  static void resetForTesting() {
    listener_().store(nullptr, std::memory_order_relaxed);
    platform_().store(kLoomPlatformAny, std::memory_order_relaxed);
    for (size_t i = 0; i < kCount; ++i) {
      store(static_cast<Id>(i), kTable[i].defaultValue);
    }
  }

 private:
  static int normalize(const LoomSettingInfo& entry, int value) {
    if (entry.type == LoomSettingType::Bool) {
      return value != 0 ? 1 : 0;
    }
    return value < entry.minimum ? entry.minimum : value;
  }

  static void store(Id id, int value) {
    values()[static_cast<size_t>(id)].store(
        normalize(info(id), value), std::memory_order_relaxed);
  }

  // Constant-initialised (no guard on the read path); keep in `kTable` order,
  // each entry its `defaultValue`.
  static std::atomic<int>* values() {
    return values_;
  }
  static inline std::atomic<int> values_[kCount]{1, 1, 1, 0, 0, 1, 50, 0, 1, 0};

  static std::atomic<uint8_t>& platform_() {
    return platformSlot_;
  }
  static inline std::atomic<uint8_t> platformSlot_{kLoomPlatformAny};

  static std::atomic<ChangeListener>& listener_() {
    return listenerSlot_;
  }
  static inline std::atomic<ChangeListener> listenerSlot_{nullptr};
};

static_assert(
    sizeof(LoomSettings::kTable) / sizeof(LoomSettingInfo) == LoomSettings::kCount,
    "one table entry per setting");

} // namespace facebook::react
