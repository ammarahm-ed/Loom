/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Loom's native API for libraries (LOOM_API_VERSION 1).
 *
 * With `LOOM_RENDERER` defined, a library talks to Loom directly instead of going through the Fabric
 * compatibility layer (which stays as the fallback for libraries that have not adopted this API).
 *
 *   #if __has_include(<react/renderer/LoomConfig.h>)
 *   #include <react/renderer/LoomConfig.h>
 *   #endif
 *   #if defined(LOOM_RENDERER) && LOOM_RENDERER
 *   #include <react/loom/Loom.h>
 *   static_assert(LOOM_API_VERSION >= 1, "needs Loom API 1");
 *   // ... facebook::react::loom::Runtime ...
 *   #else
 *   // ... Fabric ...
 *   #endif
 *
 * Concepts. A surface is one root: it owns a node tree, a Yoga tree and a mount queue. A node is a
 * retained, mutable node identified by its `Tag`; a library holding a Tag writes props to the node and
 * Loom schedules layout only if a prop affects layout (nothing is cloned or committed). A frame is the
 * layout result of a surface, applied to the views on the UI thread as one batch.
 *
 * Threading. JS thread: `Runtime::from(jsi::Runtime&)`. Any thread: reads, `updateProps`,
 * `updateAnimatedProps`, `updateNativeProps`, subscriptions. UI thread: `onFrameApplied` and
 * `beforeNextFrame` callbacks. Commit thread (the JS thread, or the Loom worker when it runs):
 * `onLayoutCommitted` callbacks; do not touch views there. With `loom.js=main` those threads are the
 * same thread and the rules above still hold; do not assume they differ.
 *
 * Value types are React Native's own (`Tag`, `SurfaceId`, `LayoutMetrics`, `Size`, `folly::dynamic`).
 * No ShadowNode, ShadowTree, ShadowNodeFamily or MountingCoordinator appears in this API.
 */

#pragma once

#if __has_include(<react/renderer/LoomConfig.h>)
#include <react/renderer/LoomConfig.h>
#endif

#if !defined(LOOM_RENDERER) || !LOOM_RENDERER
#error "<react/loom/Loom.h> needs the Loom renderer: guard the include with `#if defined(LOOM_RENDERER) && LOOM_RENDERER`."
#endif

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <folly/dynamic.h>
#include <jsi/jsi.h>
#include <react/renderer/animationbackend/AnimatedProps.h>
#include <react/renderer/core/LayoutMetrics.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/core/StateUpdate.h>
#include <react/renderer/graphics/Size.h>
#include <react/renderer/runtimescheduler/LoomSettings.h>
#include <react/utils/ContextContainer.h>

namespace facebook::react::loom {

/**
 * The typed prop bag of the animation lane: React Native's animated props (opacity, transform, colors,
 * layout props, ...), with no `folly::dynamic` on the per-frame path.
 */
using AnimatedPropBag = facebook::react::AnimatedProps;

/** What `onFrameApplied` reports. */
struct FrameInfo {
  SurfaceId surfaceId{0};
  /** When the frame's batch was applied. */
  std::chrono::steady_clock::time_point frameTime;
  /** Tags the commits since the previous applied frame of this surface changed (may be empty). */
  std::vector<Tag> changed;
};

/** RAII handle of a subscription: destroying it (or `cancel()`) unsubscribes. Movable. */
class Subscription {
 public:
  Subscription() = default;
  explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
  Subscription(Subscription&& other) noexcept : cancel_(std::move(other.cancel_)) {
    other.cancel_ = nullptr;
  }
  Subscription& operator=(Subscription&& other) noexcept {
    if (this != &other) {
      cancel();
      cancel_ = std::move(other.cancel_);
      other.cancel_ = nullptr;
    }
    return *this;
  }
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  ~Subscription() {
    cancel();
  }
  void cancel() {
    if (cancel_) {
      auto cancel = std::move(cancel_);
      cancel_ = nullptr;
      cancel();
    }
  }

 private:
  std::function<void()> cancel_;
};

/**
 * Handle to the running Loom renderer of a React instance. Cheap to copy; safe to hold on any thread.
 * Reads answer from the node tree's latest published state (a read from the JS or commit thread first
 * waits for the commits already submitted; a read on the UI thread does not block).
 */
class Runtime {
 public:
  /** From the JS runtime (JS thread). Nullopt when the app does not run the Loom renderer. */
  static std::optional<Runtime> from(jsi::Runtime& runtime);
  /** From the instance's ContextContainer (any thread), e.g. in a TurboModule provider. */
  static std::optional<Runtime> from(const ContextContainer& contextContainer);

  // ---- Surfaces ----------------------------------------------------------------------------------
  /** Calls `visit` for every running surface. Any thread. */
  void forEachSurface(const std::function<void(SurfaceId)>& visit) const;
  /** The surface's layout size in DIP. */
  std::optional<Size> surfaceSize(SurfaceId surfaceId) const;

  // ---- Nodes, by Tag -----------------------------------------------------------------------------
  bool hasNode(SurfaceId surfaceId, Tag tag) const;
  /** The node's merged props, latest. */
  std::optional<folly::dynamic> props(SurfaceId surfaceId, Tag tag) const;
  /** The node's frame in its parent. */
  std::optional<LayoutMetrics> layout(SurfaceId surfaceId, Tag tag) const;
  /** The node's frame relative to `ancestor` (nullopt: the surface root). Nullopt when `ancestor` is not one. */
  std::optional<LayoutMetrics> layoutRelativeTo(SurfaceId surfaceId, Tag tag, std::optional<Tag> ancestor) const;
  std::optional<std::string> componentName(SurfaceId surfaceId, Tag tag) const;
  std::vector<Tag> children(SurfaceId surfaceId, Tag tag) const;
  std::optional<Tag> parent(SurfaceId surfaceId, Tag tag) const;

  // ---- Prop writes: the three lanes ----------------------------------------------------------------
  /** JS lane, any thread: the same as a React commit of those props; layout runs if a prop needs it. */
  void updateProps(SurfaceId surfaceId, Tag tag, folly::dynamic props) const;
  /**
   * Animation lane, any thread: not committed, no JS work. Coalesced into the frame; a layout prop in the
   * bag makes Loom lay the node out, a paint-only bag is written to the view directly.
   */
  void updateAnimatedProps(SurfaceId surfaceId, Tag tag, AnimatedPropBag props) const;
  /** Native lane, any thread: applied to the view and remembered, so the next commit does not undo it. */
  void updateNativeProps(Tag tag, folly::dynamic props) const;

  // ---- Frames --------------------------------------------------------------------------------------
  using FrameListener = std::function<void(const FrameInfo&)>;
  using CommitListener = std::function<void(SurfaceId, const std::vector<Tag>& changed)>;
  /** `listener` runs on the UI thread right after a frame's batch was applied. */
  Subscription onFrameApplied(FrameListener listener) const;
  /** `listener` runs on the commit thread when a commit completed layout. Do not touch views there. */
  Subscription onLayoutCommitted(CommitListener listener) const;
  /** Runs `callback` once on the UI thread just before the next frame's batch of `surfaceId` is applied. */
  void beforeNextFrame(SurfaceId surfaceId, std::function<void()> callback) const;

  // ---- Components with native state (custom component descriptors) -----------------------------------
  /** Delivers a state update to the node's component. False when no node of the surface has that tag. */
  bool updateState(SurfaceId surfaceId, Tag tag, const StateUpdate& update) const;

  // ---- Settings ------------------------------------------------------------------------------------
  /** A Loom setting's value (bool settings are 0 or 1). */
  std::optional<int> setting(LoomSettingId id) const;

 private:
  Runtime() = default;
};

} // namespace facebook::react::loom
