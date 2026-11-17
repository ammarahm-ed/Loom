/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

#include "LoomRuntimeHooks.h"

namespace facebook::react {

/*
 * The frame contract. A host that has a display clock drives three things,
 * all optional; without them the scheduler uses fixed time slices and the
 * renderer applies commits as they complete:
 *
 *   - `FrameClock::onFrame(frameTimeNanos)`: once per display frame, with the
 *     frame's start time on the `std::chrono::steady_clock` epoch. Any thread.
 *   - `FrameCallbackScope`: constructed on the UI thread around the host's
 *     animation-phase callbacks (requestAnimationFrame and timer delivery),
 *     when the JS runtime shares that thread (main mode; never constructed otherwise).
 *   - `LoomBinding::flushAnimationFrames()`: the before-frame hook. The host
 *     calls it on the UI thread just before the frame's mount dispatch, so the
 *     animation props collected since the last flush reach the screen in this
 *     frame. The runtime scheduler also calls it at the end of every JS task.
 *
 * `LoomRuntimeHooks::postDelayed` is the one thing the scheduler asks of the
 * host in return.
 */

/*
 * Loom: marks the span in which the UI thread, which also hosts the
 * JavaScript runtime, is running the host's animation-phase callbacks
 * (requestAnimationFrame and timer delivery) for one frame.
 *
 * Upstream hands timer work to the runtime by posting a message to the JS
 * queue. When the host draws the frame ahead of queued messages that message
 * only runs after this frame is drawn, and the commit it makes is drawn a
 * whole frame later. Inside the scope the scheduler instead records that an
 * event loop is wanted and runs it when the scope closes, still within the
 * animation phase and therefore before the draw. A hook then lets the renderer
 * apply what the tick committed before the frame draws.
 *
 * Only meaningful on the thread that constructs it; scopes do not nest.
 */
class FrameCallbackScope final {
 public:
  FrameCallbackScope() noexcept : previous_(current_) {
    current_ = this;
  }

  ~FrameCallbackScope() noexcept {
    current_ = previous_;
    try {
      // Popped first: work scheduled while the loop runs posts normally.
      if (kick) {
        auto run = std::move(kick);
        run();
      }
      if (auto& hook = afterFrameCallbacks(); hook) {
        hook();
      }
    } catch (...) {
    }
  }

  FrameCallbackScope(const FrameCallbackScope&) = delete;
  FrameCallbackScope& operator=(const FrameCallbackScope&) = delete;

  static FrameCallbackScope* current() noexcept {
    return current_;
  }

  /** Set by the scheduler: runs the event loop that was requested in scope. */
  std::function<void()> kick;

  /** Set by the renderer: applies pending commits before the traversal. */
  static std::function<void()>& afterFrameCallbacks() {
    static std::function<void()> hook;
    return hook;
  }

 private:
  static inline thread_local FrameCallbackScope* current_ = nullptr;
  FrameCallbackScope* previous_;
};

/*
 * Loom: whether the JS work running now comes from a requestAnimationFrame
 * callback: the callback itself, or a task it scheduled, directly or through
 * other such tasks (React renders a callback's updates in a later task). The
 * scheduler carries the mark from task to task. A React commit made while it is
 * set is a frame of a JS-driven animation (LoomMountBatch::animationFrame), so
 * its text is drawn in that frame. Per thread: only the JS thread sets it.
 */
class AnimationFrameWork final {
 public:
  static void set(bool active) noexcept {
    active_ = active;
  }
  static bool active() noexcept {
    return active_;
  }

 private:
  static inline thread_local bool active_ = false;
};

/*
 * Loom: where the display's frames are, as far as the JS scheduler can tell.
 *
 * Fed with each display frame's start time (`steady_clock` epoch). It keeps a smoothed period, so it follows 60, 90 and 120 Hz
 * without asking the display, and answers "how long until the next vsync". It reports
 * "unknown" when no frame has been seen recently, which is the case when nothing animates; the
 * scheduler then falls back to a fixed time slice.
 */
class FrameClock final {
 public:
  /*
   * Tuning for splitting non-urgent work (transitions) across frames.
   *
   * kFrameReservePercent: deferrable JS stops when less than this share of the frame is left
   * before the next vsync, leaving it for the frame's animation, traversal and draw. Higher
   * means fewer dropped frames but slower landing (50% landed a 1000-cell transition in about
   * 190 ms on a 60 Hz phone, 35% in about 70 ms with a few more frames over 32 ms).
   *
   * kDeferContinuationPastVsync: true holds a paused continuation until just after the next
   * vsync. false re-posts it at once: on a host whose pending frame already holds ordinary
   * messages back (Android's sync barrier), the frame still goes first, and the gap after each
   * frame is not wasted.
   */
  static constexpr int64_t kFrameReservePercent = 35;
  static constexpr bool kDeferContinuationPastVsync = false;

  static void onFrame(int64_t frameTimeNanos) noexcept {
    const auto previous = anchor_.exchange(frameTimeNanos, std::memory_order_relaxed);
    const auto delta = frameTimeNanos - previous;
    if (previous != 0 && delta >= 4'000'000 && delta <= 40'000'000) {
      const auto period = period_.load(std::memory_order_relaxed);
      period_.store((period * 3 + delta) / 4, std::memory_order_relaxed);
    }
  }

  /** Nanoseconds to the next vsync, or INT64_MAX when frames are not being tracked. */
  static int64_t nanosUntilNextVsync() noexcept {
    const auto anchor = anchor_.load(std::memory_order_relaxed);
    if (anchor == 0) {
      return INT64_MAX;
    }
    const auto period = period_.load(std::memory_order_relaxed);
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const auto elapsed = now - anchor;
    // No frame for a while: the display is idle (or this is the first frame).
    if (elapsed < 0 || elapsed > 4 * period) {
      return INT64_MAX;
    }
    return anchor + (elapsed / period + 1) * period - now;
  }

  /**
   * True when less than kFrameReservePercent of the frame is left before the next vsync: the
   * time the frame's own animation, traversal and draw need, so that deferrable JS work stops
   * early enough for the frame to finish in time.
   */
  static bool nextVsyncIsNear() noexcept {
    const auto remaining = nanosUntilNextVsync();
    return remaining != INT64_MAX &&
        remaining < period_.load(std::memory_order_relaxed) * kFrameReservePercent / 100;
  }

  /**
   * Runs \p task just after the next vsync's frame callback has run, so a continuation that
   * the scheduler re-posts cannot be dequeued ahead of the frame that is waiting for the
   * message queue. Returns false (the caller posts normally) when frames are not tracked or no
   * `LoomRuntimeHooks::postDelayed` is installed.
   */
  static bool postAfterNextFrame(std::function<void()>& task) {
    if (!kDeferContinuationPastVsync) {
      return false;
    }
    const auto poster = LoomRuntimeHooks::installedPostDelayed();
    const auto remaining = nanosUntilNextVsync();
    if (poster == nullptr || remaining == INT64_MAX) {
      return false;
    }
    // A little past the vsync, so the frame message (stamped with the vsync time) goes first.
    poster(std::move(task), remaining + 2'000'000);
    return true;
  }

 private:
  static inline std::atomic<int64_t> anchor_{0};
  static inline std::atomic<int64_t> period_{16'666'667};
};

} // namespace facebook::react
