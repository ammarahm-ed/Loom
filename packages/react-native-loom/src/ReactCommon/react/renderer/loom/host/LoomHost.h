/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <folly/dynamic.h>
#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/attributedstring/TextAttributes.h>
#include <react/renderer/core/InstanceHandle.h>
#include <react/renderer/core/RawEvent.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/core/State.h>
#include <react/renderer/graphics/Rect.h>
#include <react/renderer/graphics/Size.h>
#include <react/renderer/loom/mounting/LoomMountBatch.h>
#include <react/renderer/loom/host/LoomPlatformTraits.h>
#include <react/renderer/loom/text/LoomTextServices.h>
#include <react/renderer/loom/text/LoomTextMeasurement.h>
#include <react/utils/ContextContainer.h>

namespace facebook::react {

/**
 * Everything a platform provides to the Loom renderer core.
 *
 * The core never touches a view, a text engine, a thread or a display. It
 * computes layout and emits ordered `LoomMountBatch`es plus a small set of
 * per-node effects; the host turns those into native views, and answers the
 * few queries that need live native state.
 *
 * Data flows the other way through static entry points on `LoomBinding`:
 *   - `setSurfaceConstraints`, `setPointScaleFactor`: viewport and density.
 *   - `releaseSurface`: the host dropped a surface.
 *   - `recordNativeProps`, `recordNativePaintProps`, `recordAnimationProps`:
 *     native and animation-backend prop writes.
 *   - `dispatchComponentState`: a native state update for a node.
 *
 * The frame source is the same kind of host-driven input, and optional. A host
 * with a display clock calls (see RuntimeSchedulerFrameCallbackScope.h):
 *   - `FrameClock::onFrame(frameTimeNanos)` once per display frame, so React's
 *     time slicing leaves room for the frame (the vsync feed);
 *   - `LoomBinding::flushAnimationFrames()` on the UI thread just before the
 *     frame's mount dispatch (the before-frame hook), so animation props
 *     collected since the last flush reach the screen in this frame;
 *   - constructs a `FrameCallbackScope` around its animation-phase callbacks
 *     (requestAnimationFrame and timer delivery) when the JS runtime shares the
 *     UI thread, so the commits they make are applied before the frame is drawn.
 * In return the scheduler may call `LoomPlatformTraits::runtime.postDelayed`.
 * A host that drives none of these still works; commits then land as they
 * complete.
 *
 * Terminology used below:
 *   - UI thread: the one thread that owns native views and applies results.
 *     `uiExecutor()` posts to it.
 *   - Commit thread: the thread a commit is computed on. It is the JS thread,
 *     or the Loom worker thread when the core runs its worker.
 *
 * Threading summary:
 *   - Any thread, never blocking on the UI thread: `uiExecutor` and the
 *     `LoomTextServices` the host returns (text is measured on the commit
 *     thread only when `LoomTextServices::canMeasureOnWorker()` is true; a
 *     commit containing text otherwise computes on the UI thread).
 *   - UI thread only: every other method. Effects recorded while a commit
 *     computes on the worker are replayed in order on the UI thread with that
 *     commit's result.
 *
 * Registration: the host is stored in the UIManager's `ContextContainer`
 * under `LoomHost::kContextKey` before the JS runtime installs the binding
 * (see `LoomHost::registerIn`); creating the binding without one is a fatal error that names `registerIn`. The
 * binding resolves the host and its text services once, when it is created.
 * The container owns the host for the life of the instance, so a host that
 * refers to platform objects holds them weakly or invalidates itself, and
 * treats calls after teardown as no-ops. Fabric's `UIManager` and `Scheduler`
 * are constructed as without Loom and carry no Loom methods.
 */
class LoomHost {
 public:
  static constexpr const char* kContextKey = "LoomHost";

  /**
   * Registers `host` and installs its `traits()` process-wide. Call before the
   * first surface starts.
   */
  static void registerIn(
      const ContextContainer& container,
      std::shared_ptr<LoomHost> host) {
    LoomPlatformTraits::install(host->traits());
    container.insert(kContextKey, std::move(host));
  }

  static std::shared_ptr<LoomHost> findIn(const ContextContainer& container) {
    return container.find<std::shared_ptr<LoomHost>>(kContextKey)
        .value_or(nullptr);
  }

  virtual ~LoomHost() noexcept = default;

  /**
   * What the core reads about this platform but does not call: component
   * vocabulary, prop-diff policy, text capabilities, logging, worker thread
   * setup. Read once, by `registerIn`. See `LoomPlatformTraits`.
   */
  virtual LoomPlatformTraits traits() const {
    return {};
  }

  // ---------------------------------------------------------------------
  // Threading
  // ---------------------------------------------------------------------

  /**
   * Posts `work` to the UI thread, FIFO, without running it inline on the
   * caller's stack. Callable from any thread and from the UI thread itself.
   * Returns an empty function when the host has no separate UI thread to post
   * to; the core then runs everything on the calling thread.
   *
   * Called once per use site, not per node. The returned function must stay
   * valid for the life of the host.
   */
  virtual std::function<void(std::function<void()>)> uiExecutor() = 0;

  // ---------------------------------------------------------------------
  // Output: mounting
  // ---------------------------------------------------------------------

  /**
   * Applies one commit's operations to the native hierarchy. UI thread.
   *
   * The batch is owned by the callee. Its operations are in the order the
   * core emitted them; `LoomMountBatch::phaseOperationIndexes` regroups them
   * into the phases below, each phase in emission order, and a host applies
   * the phases in this order (see `LoomMountBatch.h`):
   *
   *   1. CreateAndProps: `DirectCreateOperation` (tag, layoutable,
   *      componentName, props, optional parsedProps) and
   *      `DirectUpdatePropsOperation` (tag, props, optional parsedProps).
   *      `props` is the full prop set on create and the changed keys on
   *      update. `parsedProps` is set only for components with their own
   *      ComponentDescriptor; a host that builds `Props` objects uses it
   *      instead of re-parsing `props`.
   *   2. Removes: `DirectRemoveOperation` (parentTag, tag, index): detach
   *      `tag` from `parentTag`; the view stays alive until a Delete.
   *   3. Inserts: `DirectInsertOperation` (parentTag, tag, index).
   *   4. Padding: `DirectPaddingOperation` and `DirectOverflowInsetOperation`
   *      (tag, insets as left, top, right, bottom in DIP).
   *   5. Layout: `DirectLayoutOperation` (tag, parentTag, displayType,
   *      layoutDirection, x, y, width, height; frame in DIP relative to the
   *      parent).
   *   6. Deletes: `DirectDeleteOperation` (tag): destroy the view.
   *
   * `instructionCount` and `objectCount` size a host's own encoding.
   * `requiresTagReplacement` says a tag is deleted and created in the same
   * batch, so a host that keys views by tag must apply the delete first.
   * `animationFrame` marks a batch produced by an animation lane rather than a
   * React commit; text resized by it must be drawn in the same frame.
   *
   * The core treats a returned call as applied; a host that cannot apply a
   * batch reports it through its own channel.
   */
  virtual void applyBatch(SurfaceId, LoomMountBatch batch) = 0;

  /**
   * Notifies the host that a synchronous commit failed part-way through and
   * the core stopped the surface. UI thread. The host surfaces the error and
   * must not expect further batches for that surface generation.
   */
  virtual void failSurface(SurfaceId, const std::string& reason) {}

  /**
   * Applies props to a mounted view immediately, outside a batch: the
   * `setNativeProps` and animated-paint path. UI thread. `props` is the
   * flat changed-key payload; the callee does not retain the reference.
   */
  virtual void updateViewProps(Tag, const folly::dynamic& props) {}

  // ---------------------------------------------------------------------
  // Output: per-node effects. UI thread.
  // ---------------------------------------------------------------------

  /**
   * The node's JS instance handle, for building its event target. The host
   * dispatches native events for `tag` through `LoomEventManager`; the
   * handle must be retained until `unregisterEventTarget`.
   */
  virtual void registerEventTarget(SurfaceId, Tag, InstanceHandle::Shared) {}
  virtual void unregisterEventTarget(SurfaceId, Tag) {}

  /**
   * An event the core raised for `tag` (layout events and similar). The host
   * forwards it to React's event dispatcher with the given category.
   */
  virtual void dispatchEvent(
      SurfaceId,
      Tag,
      const std::string& eventName,
      folly::dynamic payload,
      RawEvent::Category category,
      bool unique) {}

  /**
   * Publishes the component's own state object (made by its
   * ComponentDescriptor) to the host view, as Fabric's UpdateState mutation
   * would.
   */
  virtual void updateComponentState(
      SurfaceId,
      Tag,
      const std::shared_ptr<const State>&) {}

  /**
   * Publishes the resolved text payload of a paragraph or text input
   * (`LoomTextStateUpdate`). The attributed string is shared, not copied.
   */
  virtual void updateTextState(const LoomTextStateUpdate&) {}

  /** Content bounds of a scroll view, in DIP, plus its scroll props. */
  virtual void updateScrollContentBounds(
      SurfaceId,
      Tag,
      const folly::dynamic& props,
      const Rect& bounds) {}

  /** A `UIManager.dispatchCommand` addressed by tag. */
  virtual void dispatchCommand(
      SurfaceId,
      Tag,
      const std::string& commandName,
      const folly::dynamic& args) {}

  virtual void sendAccessibilityEvent(
      SurfaceId,
      Tag,
      const std::string& eventType) {}

  virtual void setIsJSResponder(
      SurfaceId,
      Tag,
      bool isJSResponder,
      bool blockNativeResponder) {}

  /**
   * A native view was found inside a Text. The core calls this and then
   * `failSurface`, so a host only reports it for diagnostics.
   */
  virtual void reportUnsupportedInlineAttachment(SurfaceId, Tag) {}

  // ---------------------------------------------------------------------
  // Input: live native state. UI thread.
  //
  // When the core runs a worker, a JS-thread caller is routed through
  // `uiExecutor` and waits for the answer, so these always run on the UI
  // thread. A host that cannot answer returns the documented fallback.
  // ---------------------------------------------------------------------

  /** The live scroll position of a mounted scroll view, in DIP. */
  virtual std::pair<float, float> getScrollOffset(SurfaceId, Tag) {
    return {0, 0};
  }

  /**
   * A mounted view's origin in DIP relative to its surface root, or
   * `std::nullopt` when the view is not mounted.
   */
  virtual std::optional<std::pair<float, float>> getViewLocationInSurface(
      SurfaceId,
      Tag) {
    return std::nullopt;
  }

  // ---------------------------------------------------------------------
  // Text
  // ---------------------------------------------------------------------

  /**
   * The host's text engine (see `LoomTextServices`), or null when it has none:
   * text is then laid out with an estimate. Owned by the host and valid for
   * its lifetime. Resolved once, together with the host itself.
   */
  virtual LoomTextServices* textServices() {
    return nullptr;
  }
};

} // namespace facebook::react
