/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Loom's native API for libraries (LOOM_API_VERSION 2).
 *
 * With `LOOM_RENDERER` defined, a library talks to Loom directly instead of going through the Fabric
 * compatibility layer (which stays as the fallback for libraries that have not adopted this API).
 *
 *   #if __has_include(<react/renderer/LoomConfig.h>)
 *   #include <react/renderer/LoomConfig.h>
 *   #endif
 *   #if defined(LOOM_RENDERER) && LOOM_RENDERER
 *   #include <react/loom/Loom.h>
 *   static_assert(LOOM_API_VERSION >= 2, "needs Loom API 2");
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
 * Instances. A `Runtime` belongs to one React instance (one JS runtime). Tags are only unique within
 * an instance; every call is answered by that instance alone.
 *
 * Reads. `latest()` never blocks, on any thread: it answers from the latest computed commit, and
 * returns nullopt for everything while a commit is being computed at that instant (try again next
 * frame). `synced()` first waits until every commit submitted before the call has been applied, so the
 * JS thread reads its own writes. On the UI thread it returns nullopt (use `latest()`), except with
 * `loom.js=main`, where the UI thread is the JS thread and applies the pending commits itself first.
 * Inside an `onLayoutCommitted` listener both read the commit being reported, at once.
 *
 * Threading. JS thread: `Runtime::from(jsi::Runtime&)`. Any thread: `latest()` reads, `updateProps`,
 * `updateAnimatedProps`, `updateNativeProps`, subscriptions. `synced()`: the JS thread, or any thread
 * but the UI thread (with `loom.js=main` the JS thread is the UI thread). UI thread: `onFrameApplied` and
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

namespace internal {
struct Instance;
/** Exported entry points behind the inline `Runtime::from`, with the caller's compiled API version. */
std::shared_ptr<Instance> instanceFrom(jsi::Runtime& runtime, int libraryApiVersion);
std::shared_ptr<Instance> instanceFrom(const ContextContainer& contextContainer, int libraryApiVersion);
} // namespace internal

class Runtime;

/**
 * Node reads of one instance, in one mode (`Runtime::latest()` or `Runtime::synced()`). Every read
 * returns nullopt when it cannot be answered: no such surface or node, a commit computing right now
 * (`latest`), or a `synced` read on the UI thread.
 */
class Reads {
 public:
  /**
   * The surface of this instance whose tree has a node with `tag`, in this mode: from `synced()` it
   * includes nodes the caller's own commits just created (not mounted yet).
   */
  std::optional<SurfaceId> surfaceOf(Tag tag) const;
  /** The surface's layout size in DIP. */
  std::optional<Size> surfaceSize(SurfaceId surfaceId) const;
  std::optional<bool> hasNode(SurfaceId surfaceId, Tag tag) const;
  /** The node's merged props. */
  std::optional<folly::dynamic> props(SurfaceId surfaceId, Tag tag) const;
  /** The node's frame in its parent. */
  std::optional<LayoutMetrics> layout(SurfaceId surfaceId, Tag tag) const;
  /** The node's frame relative to `ancestor` (nullopt: the surface root). Nullopt when `ancestor` is not one. */
  std::optional<LayoutMetrics> layoutRelativeTo(SurfaceId surfaceId, Tag tag, std::optional<Tag> ancestor) const;
  std::optional<std::string> componentName(SurfaceId surfaceId, Tag tag) const;
  std::optional<std::vector<Tag>> children(SurfaceId surfaceId, Tag tag) const;
  std::optional<Tag> parent(SurfaceId surfaceId, Tag tag) const;

 private:
  friend class Runtime;
  Reads(std::shared_ptr<internal::Instance> instance, bool synced)
      : instance_(std::move(instance)), synced_(synced) {}
  std::shared_ptr<internal::Instance> instance_;
  bool synced_;
};

/**
 * Handle to the Loom renderer of one React instance. Cheap to copy; safe to hold on any thread. Calls
 * on a handle whose instance was torn down do nothing and return nullopt.
 */
class Runtime {
 public:
  /** From the JS runtime (JS thread). Nullopt when the app does not run the Loom renderer. */
  static std::optional<Runtime> from(jsi::Runtime& runtime) {
    return wrap(internal::instanceFrom(runtime, LOOM_API_VERSION));
  }
  /** From the instance's ContextContainer (any thread), e.g. in a TurboModule provider. */
  static std::optional<Runtime> from(const ContextContainer& contextContainer) {
    return wrap(internal::instanceFrom(contextContainer, LOOM_API_VERSION));
  }

  // ---- Surfaces ----------------------------------------------------------------------------------
  /** Calls `visit` for every running surface of this instance. Any thread. */
  void forEachSurface(const std::function<void(SurfaceId)>& visit) const;
  /**
   * The surface of this instance on which a node with `tag` is mounted (on screen). Any thread, never
   * blocks. A node the JS thread just created is not mounted yet: use `synced().surfaceOf(tag)`.
   */
  std::optional<SurfaceId> surfaceOf(Tag tag) const;

  // ---- Reads ---------------------------------------------------------------------------------------
  /** Never blocks; any thread. See "Reads" above. */
  Reads latest() const {
    return Reads(instance_, /*synced=*/false);
  }
  /** Waits for the caller's submitted commits; never on the UI thread. See "Reads" above. */
  Reads synced() const {
    return Reads(instance_, /*synced=*/true);
  }

  // ---- Prop writes: the three lanes ----------------------------------------------------------------
  /** JS lane, any thread: the same as a React commit of those props; layout runs if a prop needs it. */
  void updateProps(SurfaceId surfaceId, Tag tag, folly::dynamic props) const;
  /**
   * Animation lane, any thread: not committed, no JS work. Coalesced into the frame; a layout prop in the
   * bag makes Loom lay the node out, a paint-only bag is written to the view directly.
   */
  void updateAnimatedProps(SurfaceId surfaceId, Tag tag, AnimatedPropBag props) const;
  /** Native lane, any thread: applied to the view and remembered, so the next commit does not undo it. */
  void updateNativeProps(SurfaceId surfaceId, Tag tag, folly::dynamic props) const;

  // ---- Frames --------------------------------------------------------------------------------------
  using FrameListener = std::function<void(const FrameInfo&)>;
  using CommitListener = std::function<void(SurfaceId, const std::vector<Tag>& changed)>;
  /** `listener` runs on the UI thread right after a frame's batch of this instance was applied. */
  Subscription onFrameApplied(FrameListener listener) const;
  /** `listener` runs on the commit thread when a commit of this instance completed layout. Do not touch views there. */
  Subscription onLayoutCommitted(CommitListener listener) const;
  /** Runs `callback` once on the UI thread just before the next batch of `surfaceId` is applied. */
  void beforeNextFrame(SurfaceId surfaceId, std::function<void()> callback) const;

  // ---- Components with native state (custom component descriptors) -----------------------------------
  /** Delivers a state update to the node's component. False when no node of the surface has that tag. */
  bool updateState(SurfaceId surfaceId, Tag tag, const StateUpdate& update) const;

  // ---- Settings ------------------------------------------------------------------------------------
  /** A Loom setting's value (bool settings are 0 or 1). */
  std::optional<int> setting(LoomSettingId id) const;

 private:
  explicit Runtime(std::shared_ptr<internal::Instance> instance) : instance_(std::move(instance)) {}
  static std::optional<Runtime> wrap(std::shared_ptr<internal::Instance> instance) {
    if (instance == nullptr) {
      return std::nullopt;
    }
    return Runtime(std::move(instance));
  }
  // The only member: the handle's size and layout never change as the API grows.
  std::shared_ptr<internal::Instance> instance_;
};

} // namespace facebook::react::loom
