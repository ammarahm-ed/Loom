/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/cxxstableapi/FrameworksGuard.h>

#include <atomic>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

#include <React/ComponentRegistry.h>
#include <React/RendererCore.h>
#include <React/RuntimeExecutor.h>
#include <React/Utils.h>
#include <react/renderer/mounting/MountingOverrideDelegate.h>
#include <react/renderer/scheduler/SchedulerDelegate.h>
#include <react/renderer/scheduler/SchedulerToolbox.h>
#include <react/renderer/scheduler/SurfaceHandler.h>
#include <react/renderer/uimanager/UIManagerAnimationDelegate.h>
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/uimanager/UIManagerDelegate.h>

namespace facebook::react {

class CdpMetricsReporter;
class CdpPerfIssuesReporter;
class EventPerformanceLogger;
class PerformanceEntryReporter;
class ViewTransitionModule;

/*
 * Scheduler coordinates Shadow Tree updates and event flows.
 */
class Scheduler final : public UIManagerDelegate {
 public:
  Scheduler(
      const SchedulerToolbox &schedulerToolbox,
      UIManagerAnimationDelegate *animationDelegate,
      SchedulerDelegate *delegate);
  ~Scheduler() override;

#pragma mark - Surface Management

  /*
   * Registers and unregisters a `SurfaceHandler` object in the `Scheduler`.
   */
  void registerSurface(const SurfaceHandler &surfaceHandler) const noexcept;
  void unregisterSurface(const SurfaceHandler &surfaceHandler) const noexcept;

  /*
   * This is broken. Please do not use.
   * `ComponentDescriptor`s are not designed to be used outside of `UIManager`,
   * there is no any guarantees about their lifetime.
   */
  const ComponentDescriptor *findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN(ComponentHandle handle) const;
  SharedComponentDescriptorRegistry getComponentDescriptorRegistry() const;

#pragma mark - Delegate

  /*
   * Sets and gets the Scheduler's delegate.
   * If you requesting a ComponentDescriptor and unsure that it's there, you are
   * doing something wrong.
   */
  void setDelegate(SchedulerDelegate *delegate);
  SchedulerDelegate *getDelegate() const;
  EventDispatcher::Shared getEventDispatcher() const;

#pragma mark - UIManagerAnimationDelegate
  // This is not needed on iOS or any platform that has a "pull" instead of
  // "push" MountingCoordinator model. This just tells the delegate an update
  // is available and that it should `pullTransaction`; we may want to rename
  // this to be more generic and not animation-specific.
  void animationTick() const;

#pragma mark - UIManagerDelegate

  void uiManagerDidFinishTransaction(
      std::shared_ptr<const MountingCoordinator> mountingCoordinator,
      bool mountSynchronously) override;
#if UI_THREAD_RENDERER
  void uiThreadRendererSetConstraints(
      SurfaceId,
      float,
      float,
      float,
      float,
      bool,
      float,
      float) override;
  std::function<void(std::function<void()>)> uiThreadRendererGetUIExecutor() override;
  bool uiThreadRendererCanMeasureTextOnWorker() override;
  void uiThreadRendererApplyBatch(SurfaceId, UIThreadDirectMountBatch) override;
  void uiThreadRendererFailSurface(SurfaceId, const std::string&) override;
  void uiThreadRendererRegisterEventTarget(SurfaceId, Tag, InstanceHandle::Shared) override;
  void uiThreadRendererUnregisterEventTarget(SurfaceId, Tag) override;
  void uiThreadRendererDispatchEvent(
      SurfaceId, Tag, const std::string&, folly::dynamic, RawEvent::Category, bool) override;
  TextAttributes uiThreadRendererParseTextAttributes(
      SurfaceId,
      const std::string&,
      const folly::dynamic&,
      const TextAttributes&,
      ParagraphAttributes&) override;
  std::optional<Size> uiThreadRendererMeasureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) override;
  UIThreadTextMeasurement uiThreadRendererPrepareText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) override;
  UIThreadTextMeasurement uiThreadRendererPrepareTextForMount(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size&,
      const Size&) override;
  void uiThreadRendererUpdateState(
      SurfaceId,
      Tag,
      const std::string&,
      const folly::dynamic&,
      const AttributedString&,
      const ParagraphAttributes&,
      const UIThreadTextMeasurement&) override;
  void uiThreadRendererUpdateScrollContentBounds(
      SurfaceId,
      Tag,
      const folly::dynamic&,
      const Rect&) override;
  std::pair<float, float> uiThreadRendererGetScrollOffset(SurfaceId, Tag)
      override;
  std::optional<std::pair<float, float>>
  uiThreadRendererGetViewLocationInSurface(SurfaceId, Tag) override;
  void uiThreadRendererDispatchCommand(
      SurfaceId, Tag, const std::string&, const folly::dynamic&) override;
  void uiThreadRendererSendAccessibilityEvent(
      SurfaceId, Tag, const std::string&) override;
  void uiThreadRendererSetIsJSResponder(SurfaceId, Tag, bool, bool) override;
#endif
  void uiManagerDidCreateShadowNode(const ShadowNode &shadowNode) override;
  void uiManagerDidDispatchCommand(
      const std::shared_ptr<const ShadowNode> &shadowNode,
      const std::string &commandName,
      const folly::dynamic &args) override;
  void uiManagerDidSendAccessibilityEvent(
      const std::shared_ptr<const ShadowNode> &shadowNode,
      const std::string &eventType) override;
  void uiManagerDidSetIsJSResponder(
      const std::shared_ptr<const ShadowNode> &shadowNode,
      bool isJSResponder,
      bool blockNativeResponder) override;
  void uiManagerShouldSynchronouslyUpdateViewOnUIThread(Tag tag, const folly::dynamic &props) override;
  void uiManagerShouldSynchronouslyUpdateAnimatedProps(const std::unordered_map<Tag, AnimatedProps> &updates) override;
  void uiManagerDidUpdateShadowTree(const std::unordered_map<Tag, folly::dynamic> &tagToProps) override;
  void uiManagerDidCaptureViewSnapshot(Tag tag, SurfaceId surfaceId) override;
  void uiManagerDidSetViewSnapshot(Tag sourceTag, Tag targetTag, SurfaceId surfaceId) override;
  void uiManagerDidClearPendingSnapshots() override;
  void uiManagerShouldAddEventListener(std::shared_ptr<const EventListener> listener) final;
  void uiManagerShouldRemoveEventListener(const std::shared_ptr<const EventListener> &listener) final;
  void uiManagerDidFinishReactCommit(const ShadowTree &shadowTree) override;
  void uiManagerDidStartSurface(const ShadowTree &shadowTree) override;

#pragma mark - ContextContainer
  std::shared_ptr<const ContextContainer> getContextContainer() const;

#pragma mark - UIManager
  std::shared_ptr<UIManager> getUIManager() const;

  void reportMount(SurfaceId surfaceId) const;

#pragma mark - Event listeners
  void addEventListener(std::shared_ptr<const EventListener> listener);
  void removeEventListener(const std::shared_ptr<const EventListener> &listener);

#pragma mark - Surface start callback
  void uiManagerShouldAddOnSurfaceStartCallback(OnSurfaceStartCallback &&callback) override;

 private:
  friend class SurfaceHandler;

  SchedulerDelegate *delegate_;
  SharedComponentDescriptorRegistry componentDescriptorRegistry_;
  RuntimeExecutor runtimeExecutor_;
  std::shared_ptr<UIManager> uiManager_;

  std::vector<std::shared_ptr<UIManagerCommitHook>> commitHooks_;

  /*
   * At some point, we have to have an owning shared pointer to something that
   * will become an `EventDispatcher` a moment later. That's why we have it as a
   * pointer to an optional: we construct the pointer first, share that with
   * parts that need to have ownership (and only ownership) of that, and then
   * fill the optional.
   */
  std::shared_ptr<std::optional<const EventDispatcher>> eventDispatcher_;

  std::shared_ptr<PerformanceEntryReporter> performanceEntryReporter_;
  std::unique_ptr<CdpMetricsReporter> cdpMetricsReporter_;
  std::unique_ptr<CdpPerfIssuesReporter> cdpPerfIssuesReporter_;
  std::shared_ptr<EventPerformanceLogger> eventPerformanceLogger_;

  /**
   * Hold onto ContextContainer. See SchedulerToolbox.
   * Must not be nullptr.
   */
  std::shared_ptr<const ContextContainer> contextContainer_;

  RuntimeScheduler *runtimeScheduler_{nullptr};

  std::shared_ptr<ViewTransitionModule> viewTransitionModule_;

  mutable std::shared_mutex onSurfaceStartCallbackMutex_;
  std::vector<OnSurfaceStartCallback> onSurfaceStartCallbacks_;
};

} // namespace facebook::react
