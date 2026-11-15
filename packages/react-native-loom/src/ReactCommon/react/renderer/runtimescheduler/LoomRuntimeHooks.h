/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace facebook::react {

/**
 * The host services the runtime scheduler layer calls. It has no dependencies
 * so that layer can use it without the renderer. A host fills one in and
 * installs it (`LoomPlatformTraits::install` does it for `traits.runtime`);
 * `install` merges, setting each hook the instance provides and leaving the
 * rest as they were, so independent parts of a host may install their own.
 */
struct LoomRuntimeHooks {
  using PostDelayed = void (*)(std::function<void()> task, int64_t delayNanos);

  /**
   * Runs `task` on the UI thread after `delayNanos`. Used to re-post a
   * paused JS continuation just after the next frame. Null: the scheduler
   * posts normally.
   */
  PostDelayed postDelayed{nullptr};

  static void install(const LoomRuntimeHooks& hooks) {
    if (hooks.postDelayed != nullptr) {
      postDelayedSlot_.store(hooks.postDelayed, std::memory_order_release);
    }
  }

  static PostDelayed installedPostDelayed() {
    return postDelayedSlot_.load(std::memory_order_acquire);
  }

 private:
  // Process-wide state lives in `static inline` data members, never in a function-local
  // `static` of an inline function: with hidden inline functions (Xcode's default) and
  // per-target prelinking (`ld -r`, as SwiftPM builds package targets), each target keeps
  // its own copy of such a static, so a value installed by one target is invisible to
  // another. Inline variables keep default visibility and stay one definition.
  static inline std::atomic<PostDelayed> postDelayedSlot_{nullptr};
};

} // namespace facebook::react
