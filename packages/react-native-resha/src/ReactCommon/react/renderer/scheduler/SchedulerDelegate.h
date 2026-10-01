/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>

#include <folly/dynamic.h>

#include <optional>

#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/uimanager/UIThreadTextMeasurement.h>
#include <react/renderer/uimanager/UIThreadDirectMountBatch.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/core/EventEmitter.h>
#include <react/renderer/core/RawEvent.h>
#include <react/renderer/graphics/Rect.h>
#include <react/renderer/mounting/MountingCoordinator.h>
#include <react/renderer/mounting/ShadowView.h>

namespace facebook::react {

/*
 * Abstract class for Scheduler's delegate.
 */
class SchedulerDelegate {
 public:
#if UI_THREAD_RENDERER
  // Forwarded straight through from `UIManagerDelegate`; see that header for
  // the meaning of each one.
  virtual void uiThreadRendererSetConstraints(
      SurfaceId,
      float,
      float,
      float,
      float,
      bool,
      float,
      float) {}
  virtual std::function<void(std::function<void()>)> uiThreadRendererGetUIExecutor() { return {}; }
  virtual bool uiThreadRendererCanMeasureTextOnWorker() { return false; }
  virtual void uiThreadRendererApplyBatch(
      SurfaceId,
      UIThreadDirectMountBatch) {}
  virtual void uiThreadRendererFailSurface(SurfaceId, const std::string&) {}
  virtual void uiThreadRendererRegisterEventTarget(SurfaceId, Tag, InstanceHandle::Shared) {}
  virtual void uiThreadRendererUnregisterEventTarget(SurfaceId, Tag) {}
  virtual void uiThreadRendererDispatchEvent(
      SurfaceId, Tag, const std::string&, folly::dynamic, RawEvent::Category, bool) {}
  virtual TextAttributes uiThreadRendererParseTextAttributes(
      SurfaceId,
      const std::string&,
      const folly::dynamic&,
      const TextAttributes& inherited,
      ParagraphAttributes&) {
    return inherited;
  }
  virtual std::optional<Size> uiThreadRendererMeasureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return std::nullopt;
  }
  virtual UIThreadTextMeasurement uiThreadRendererPrepareText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return {};
  }
  virtual UIThreadTextMeasurement uiThreadRendererPrepareTextForMount(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return {};
  }
  virtual void uiThreadRendererUpdateState(
      SurfaceId,
      Tag,
      const std::string&,
      const folly::dynamic&,
      const AttributedString&,
      const ParagraphAttributes&,
      const UIThreadTextMeasurement&) {}
  virtual void uiThreadRendererUpdateScrollContentBounds(
      SurfaceId,
      Tag,
      const folly::dynamic&,
      const Rect&) {}
  virtual std::pair<float, float> uiThreadRendererGetScrollOffset(
      SurfaceId,
      Tag) {
    return {0, 0};
  }
  virtual std::optional<std::pair<float, float>>
  uiThreadRendererGetViewLocationInSurface(SurfaceId, Tag) {
    return std::nullopt;
  }
  virtual void uiThreadRendererDispatchCommand(
      SurfaceId, Tag, const std::string&, const folly::dynamic&) {}
  virtual void uiThreadRendererSendAccessibilityEvent(
      SurfaceId, Tag, const std::string&) {}
  virtual void uiThreadRendererSetIsJSResponder(SurfaceId, Tag, bool, bool) {}
#endif
  /*
   * Called right after Scheduler computed (and laid out) a new updated version
   * of the tree and calculated a set of mutations which are sufficient
   * to construct a new one.
   */
  virtual void schedulerDidFinishTransaction(const std::shared_ptr<const MountingCoordinator> &mountingCoordinator) = 0;

  /*
   * Called when the runtime scheduler decides that one-or-more previously
   * finished transactions should now be flushed to the screen (atomically).
   *
   * This is a separate callback from didFinishTransaction as the Android UI
   * mounting layer needs to be able toobserve each created ShadowTree to
   * correctly apply changes, due to changes in Props representation.
   */
  virtual void schedulerShouldRenderTransactions(
      const std::shared_ptr<const MountingCoordinator> &mountingCoordinator) = 0;

  /*
   * Called at the end of the event loop if there were commits to the JS
   * during the pass and JS branch should be "merged" to the main revision.
   */
  virtual void schedulerShouldMergeReactRevision(SurfaceId surfaceId) = 0;

  /*
   * Called right after a new ShadowNode was created.
   */
  virtual void schedulerDidRequestPreliminaryViewAllocation(const ShadowNode &shadowNode) = 0;

  virtual void schedulerDidDispatchCommand(
      const ShadowView &shadowView,
      const std::string &commandName,
      const folly::dynamic &args) = 0;

  virtual void schedulerDidSendAccessibilityEvent(const ShadowView &shadowView, const std::string &eventType) = 0;

  /*
   * Set JS responder for a view
   */
  virtual void
  schedulerDidSetIsJSResponder(const ShadowView &shadowView, bool isJSResponder, bool blockNativeResponder) = 0;

  virtual void schedulerShouldSynchronouslyUpdateViewOnUIThread(Tag tag, const folly::dynamic &props) = 0;

  virtual void schedulerDidUpdateShadowTree(const std::unordered_map<Tag, folly::dynamic> &tagToProps) = 0;

  // View transition bitmap snapshot capture and application.
  virtual void schedulerDidCaptureViewSnapshot(Tag tag, SurfaceId surfaceId) = 0;
  virtual void schedulerDidSetViewSnapshot(Tag sourceTag, Tag targetTag, SurfaceId surfaceId) = 0;
  virtual void schedulerDidClearPendingSnapshots() = 0;

  virtual ~SchedulerDelegate() noexcept = default;
};

} // namespace facebook::react
