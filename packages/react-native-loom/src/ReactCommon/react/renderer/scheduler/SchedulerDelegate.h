/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/FrameworksGuard.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <React/Graphics.h>
#include <React/RendererCore.h>
#include <folly/dynamic.h>
#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/uimanager/LoomMountBatch.h>
#include <react/renderer/uimanager/LoomTextMeasurement.h>
#include <react/renderer/mounting/MountingCoordinator.h>
#include <react/renderer/mounting/ShadowView.h>

namespace facebook::react {

struct AnimatedProps;

/*
 * Abstract class for Scheduler's delegate.
 */
class SchedulerDelegate {
 public:
// Loom delegate methods: declared in every build so the class has one layout for code compiled without LOOM_RENDERER (third-party libraries).
  // Forwarded straight through from `UIManagerDelegate`; see that header for
  // the meaning of each one.
  virtual void loomSetConstraints(
      SurfaceId,
      float,
      float,
      float,
      float,
      bool,
      float,
      float) {}
  virtual std::function<void(std::function<void()>)> loomGetUIExecutor() { return {}; }
  virtual bool loomCanMeasureTextOnWorker() { return false; }
  virtual void loomApplyBatch(
      SurfaceId,
      LoomMountBatch) {}
  virtual void loomFailSurface(SurfaceId, const std::string&) {}
  virtual void loomRegisterEventTarget(SurfaceId, Tag, InstanceHandle::Shared) {}
  virtual void loomUnregisterEventTarget(SurfaceId, Tag) {}
  virtual void loomDispatchEvent(
      SurfaceId, Tag, const std::string&, folly::dynamic, RawEvent::Category, bool) {}
  virtual TextAttributes loomParseTextAttributes(
      SurfaceId,
      const std::string&,
      const folly::dynamic&,
      const TextAttributes& inherited,
      ParagraphAttributes&) {
    return inherited;
  }
  virtual std::optional<Size> loomMeasureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return std::nullopt;
  }
  virtual LoomTextMeasurement loomPrepareText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return {};
  }
  virtual LoomTextMeasurement loomPrepareTextForMount(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) {
    return {};
  }
  virtual void loomUpdateState(const LoomTextStateUpdate&) {}
  virtual void loomUpdateComponentState(
      SurfaceId,
      Tag,
      const std::shared_ptr<const State>&) {}
  virtual void loomUpdateScrollContentBounds(
      SurfaceId,
      Tag,
      const folly::dynamic&,
      const Rect&) {}
  virtual std::pair<float, float> loomGetScrollOffset(
      SurfaceId,
      Tag) {
    return {0, 0};
  }
  virtual std::optional<std::pair<float, float>>
  loomGetViewLocationInSurface(SurfaceId, Tag) {
    return std::nullopt;
  }
  virtual void loomDispatchCommand(
      SurfaceId, Tag, const std::string&, const folly::dynamic&) {}
  virtual void loomSendAccessibilityEvent(
      SurfaceId, Tag, const std::string&) {}
  virtual void loomSetIsJSResponder(SurfaceId, Tag, bool, bool) {}
// (end of Loom delegate methods)
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

  /*
   * Synchronous update of animated props for many views at once. By default,
   * forwards each view to `schedulerShouldSynchronouslyUpdateViewOnUIThread`.
   */
  virtual void schedulerShouldSynchronouslyUpdateAnimatedProps(const std::unordered_map<Tag, AnimatedProps> &updates);

  virtual void schedulerDidUpdateShadowTree(const std::unordered_map<Tag, folly::dynamic> &tagToProps) = 0;

  // View transition bitmap snapshot capture and application.
  virtual void schedulerDidCaptureViewSnapshot(Tag tag, SurfaceId surfaceId) = 0;
  virtual void schedulerDidSetViewSnapshot(Tag sourceTag, Tag targetTag, SurfaceId surfaceId) = 0;
  virtual void schedulerDidClearPendingSnapshots() = 0;

  virtual ~SchedulerDelegate() noexcept = default;
};

} // namespace facebook::react
