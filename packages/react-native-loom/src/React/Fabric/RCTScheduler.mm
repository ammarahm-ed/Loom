/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import "RCTScheduler.h"

#import <react/renderer/animationbackend/AnimatedProps.h>

#import <QuartzCore/CADisplayLink.h>
#import <cxxreact/TraceSection.h>
#import <react/featureflags/ReactNativeFeatureFlags.h>
#import <react/renderer/animations/LayoutAnimationDriver.h>
#import <react/renderer/componentregistry/ComponentDescriptorFactory.h>
#import <react/renderer/scheduler/Scheduler.h>
#import <react/renderer/scheduler/SchedulerDelegate.h>
#import <react/utils/RunLoopObserver.h>

#import "PlatformRunLoopObserver.h"
#import "RCTConversions.h"
#if LOOM_RENDERER
#import <react/renderer/uimanager/LoomBinding.h>
#endif

using namespace facebook::react;

class SchedulerDelegateProxy : public SchedulerDelegate {
 public:
  SchedulerDelegateProxy(void *scheduler) : scheduler_(scheduler) {}

  void schedulerDidFinishTransaction(const std::shared_ptr<const MountingCoordinator> &mountingCoordinator) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDidFinishTransaction:mountingCoordinator];
  }

  void schedulerShouldRenderTransactions(const std::shared_ptr<const MountingCoordinator> &mountingCoordinator) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerShouldRenderTransactions:mountingCoordinator];
  }

  void schedulerShouldMergeReactRevision(SurfaceId surfaceId) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerShouldMergeReactRevision:surfaceId];
  }

  void schedulerDidRequestPreliminaryViewAllocation(const ShadowNode &shadowNode) override
  {
    // Does nothing.
    // This delegate method is not currently used on iOS.
  }

  void schedulerDidDispatchCommand(
      const ShadowView &shadowView,
      const std::string &commandName,
      const folly::dynamic &args) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDidDispatchCommand:shadowView commandName:commandName args:args];
  }

  void schedulerDidSetIsJSResponder(const ShadowView &shadowView, bool isJSResponder, bool blockNativeResponder)
      override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDidSetIsJSResponder:isJSResponder
                                blockNativeResponder:blockNativeResponder
                                       forShadowView:shadowView];
  }

  void schedulerDidSendAccessibilityEvent(const ShadowView &shadowView, const std::string &eventType) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDidSendAccessibilityEvent:shadowView eventType:eventType];
  }

  void schedulerShouldSynchronouslyUpdateViewOnUIThread(facebook::react::Tag tag, const folly::dynamic &props) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDidSynchronouslyUpdateViewOnUIThread:tag props:props];
  }

#if LOOM_RENDERER
  // Loom: a frame of the shared animation backend arrives as one typed batch
  // (paint-only views); the presenter writes it without packing and re-parsing
  // a props dictionary per view.
  void schedulerShouldSynchronouslyUpdateAnimatedProps(
      const std::unordered_map<facebook::react::Tag, facebook::react::AnimatedProps> &updates) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDidSynchronouslyUpdateAnimatedProps:updates];
  }
#endif

  void schedulerDidUpdateShadowTree(const std::unordered_map<Tag, folly::dynamic> &tagToProps) override
  {
    // Does nothing.
    // This delegate method is not currently used on iOS.
  }

  void schedulerDidCaptureViewSnapshot(Tag tag, SurfaceId surfaceId) override
  {
    // Does nothing.
    // View transition snapshots are not currently implemented on iOS.
  }

  void schedulerDidSetViewSnapshot(Tag sourceTag, Tag targetTag, SurfaceId surfaceId) override
  {
    // Does nothing.
    // View transition snapshots are not currently implemented on iOS.
  }

  void schedulerDidClearPendingSnapshots() override
  {
    // Does nothing.
    // View transition snapshots are not currently implemented on iOS.
  }

#if LOOM_RENDERER
  void loomSetConstraints(
      SurfaceId surfaceId,
      float minWidth,
      float minHeight,
      float maxWidth,
      float maxHeight,
      bool isRTL,
      float offsetX,
      float offsetY) override
  {
    LoomBinding::setSurfaceConstraints(
        surfaceId, minWidth, minHeight, maxWidth, maxHeight, isRTL, offsetX, offsetY);
  }

  std::function<void(std::function<void()>)> loomGetUIExecutor() override
  {
    return [](std::function<void()> callback) {
      dispatch_async(dispatch_get_main_queue(), ^{ callback(); });
    };
  }

  bool loomCanMeasureTextOnWorker() override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    return [scheduler.delegate schedulerDirectRendererCanMeasureTextOnWorker];
  }

  void loomApplyBatch(SurfaceId surfaceId, LoomMountBatch batch) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererApplyBatch:surfaceId batch:std::move(batch)];
  }

  void loomFailSurface(SurfaceId surfaceId, const std::string &reason) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererFailSurface:surfaceId reason:reason];
  }

  void loomRegisterEventTarget(SurfaceId surfaceId, Tag tag, InstanceHandle::Shared instanceHandle) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererRegisterEventTarget:surfaceId tag:tag instanceHandle:std::move(instanceHandle)];
  }

  void loomUnregisterEventTarget(SurfaceId surfaceId, Tag tag) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererUnregisterEventTarget:surfaceId tag:tag];
  }

  void loomDispatchEvent(
      SurfaceId surfaceId,
      Tag tag,
      const std::string &name,
      folly::dynamic payload,
      RawEvent::Category category,
      bool unique) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererDispatchEvent:surfaceId
                                                        tag:tag
                                                      name:name
                                                    payload:std::move(payload)
                                                   category:category
                                                     unique:unique];
  }

  TextAttributes loomParseTextAttributes(
      SurfaceId surfaceId,
      const std::string &name,
      const folly::dynamic &props,
      const TextAttributes &inherited,
      ParagraphAttributes &paragraphAttributes) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    return [scheduler.delegate schedulerDirectRendererParseTextAttributes:surfaceId
                                                                     name:name
                                                                    props:props
                                                                inherited:inherited
                                                       paragraphAttributes:paragraphAttributes];
  }

  std::optional<facebook::react::Size> loomMeasureText(
      SurfaceId surfaceId,
      const AttributedString &attributedString,
      const ParagraphAttributes &paragraphAttributes,
      const facebook::react::Size &minimumSize,
      const facebook::react::Size &maximumSize) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    return [scheduler.delegate schedulerDirectRendererMeasureText:surfaceId
                                                attributedString:attributedString
                                             paragraphAttributes:paragraphAttributes
                                                    minimumSize:minimumSize
                                                    maximumSize:maximumSize];
  }

  LoomTextMeasurement loomPrepareText(
      SurfaceId surfaceId,
      const AttributedString &attributedString,
      const ParagraphAttributes &paragraphAttributes,
      const facebook::react::Size &minimumSize,
      const facebook::react::Size &maximumSize) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    return [scheduler.delegate schedulerDirectRendererPrepareText:surfaceId
                                                attributedString:attributedString
                                             paragraphAttributes:paragraphAttributes
                                                    minimumSize:minimumSize
                                                    maximumSize:maximumSize];
  }

  LoomTextMeasurement loomPrepareTextForMount(
      SurfaceId surfaceId,
      const AttributedString &attributedString,
      const ParagraphAttributes &paragraphAttributes,
      const facebook::react::Size &minimumSize,
      const facebook::react::Size &maximumSize) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    return [scheduler.delegate schedulerDirectRendererPrepareTextForMount:surfaceId
                                                        attributedString:attributedString
                                                     paragraphAttributes:paragraphAttributes
                                                            minimumSize:minimumSize
                                                            maximumSize:maximumSize];
  }

  void loomUpdateState(const LoomTextStateUpdate &update) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererUpdateTextState:update];
  }

  void loomUpdateComponentState(SurfaceId surfaceId, Tag tag, const std::shared_ptr<const State> &state) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererUpdateComponentState:surfaceId tag:tag state:state];
  }

  void loomUpdateScrollContentBounds(
      SurfaceId surfaceId,
      Tag tag,
      const folly::dynamic &props,
      const facebook::react::Rect &bounds) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererUpdateScrollContentBounds:surfaceId
                                                                       tag:tag
                                                                      props:props
                                                                     bounds:bounds];
  }

  void loomDispatchCommand(
      SurfaceId surfaceId,
      Tag tag,
      const std::string &name,
      const folly::dynamic &args) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererDispatchCommand:surfaceId tag:tag name:name args:args];
  }

  void loomSendAccessibilityEvent(SurfaceId surfaceId, Tag tag, const std::string &name) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererAccessibilityEvent:surfaceId tag:tag name:name];
  }

  std::optional<std::pair<float, float>> loomGetViewLocationInSurface(SurfaceId surfaceId, Tag tag) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    CGPoint location;
    if (![scheduler.delegate schedulerDirectRendererViewLocationInSurface:surfaceId tag:tag location:&location]) {
      return std::nullopt;
    }
    return std::pair<float, float>{static_cast<float>(location.x), static_cast<float>(location.y)};
  }

  void loomSetIsJSResponder(SurfaceId surfaceId, Tag tag, bool isResponder, bool blockNativeResponder) override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler.delegate schedulerDirectRendererSetJSResponder:surfaceId
                                                         tag:tag
                                                   isResponder:isResponder
                                          blockNativeResponder:blockNativeResponder];
  }

#endif

 private:
  void *scheduler_;
};

class LayoutAnimationDelegateProxy : public LayoutAnimationStatusDelegate, public RunLoopObserver::Delegate {
 public:
  LayoutAnimationDelegateProxy(void *scheduler) : scheduler_(scheduler) {}
  virtual ~LayoutAnimationDelegateProxy() {}

  void onAnimationStarted() override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler onAnimationStarted];
  }

  /**
   * Called when the LayoutAnimation engine completes all pending animations.
   */
  void onAllAnimationsComplete() override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler onAllAnimationsComplete];
  }

  void activityDidChange(const RunLoopObserver::Delegate *delegate, RunLoopObserver::Activity activity)
      const noexcept override
  {
    RCTScheduler *scheduler = (__bridge RCTScheduler *)scheduler_;
    [scheduler animationTick];
  }

 private:
  void *scheduler_;
};

@interface RCTAnimationChoreographerDisplayLinkTarget : NSObject
@property (nonatomic, assign) AnimationChoreographer *choreographer;
- (void)displayLinkTick:(CADisplayLink *)sender;
@end

@implementation RCTAnimationChoreographerDisplayLinkTarget
- (void)displayLinkTick:(CADisplayLink *)sender
{
  if (_choreographer != nullptr) {
    _choreographer->onAnimationFrame(std::chrono::duration<double>(sender.targetTimestamp));
  }
}
@end

class RCTAnimationChoreographer : public AnimationChoreographer {
  CADisplayLink *_animationDisplayLink;
  RCTAnimationChoreographerDisplayLinkTarget *_displayLinkTarget;

 public:
  RCTAnimationChoreographer() : _displayLinkTarget([[RCTAnimationChoreographerDisplayLinkTarget alloc] init])
  {
    _displayLinkTarget.choreographer = this;
  }
  ~RCTAnimationChoreographer() override
  {
    if (_animationDisplayLink != nil) {
      [_animationDisplayLink invalidate];
      _animationDisplayLink = nil;
    }
    _displayLinkTarget.choreographer = nullptr;
  }
  void resume() override
  {
    if (_animationDisplayLink == nil) {
      _animationDisplayLink = [CADisplayLink displayLinkWithTarget:_displayLinkTarget
                                                          selector:@selector(displayLinkTick:)];
      [_animationDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
    [_animationDisplayLink setPaused:NO];
  }
  void pause() override
  {
    [_animationDisplayLink setPaused:YES];
  }
};

@implementation RCTScheduler {
  std::unique_ptr<Scheduler> _scheduler;
  std::shared_ptr<LayoutAnimationDriver> _animationDriver;
  std::unique_ptr<SchedulerDelegateProxy> _delegateProxy;
  std::shared_ptr<LayoutAnimationDelegateProxy> _layoutAnimationDelegateProxy;
  std::unique_ptr<const PlatformRunLoopObserver> _uiRunLoopObserver;
}

- (instancetype)initWithToolbox:(SchedulerToolbox)toolbox
{
  if (self = [super init]) {
    _delegateProxy = std::make_unique<SchedulerDelegateProxy>((__bridge void *)self);

    if (ReactNativeFeatureFlags::enableLayoutAnimationsOnIOS()) {
      _layoutAnimationDelegateProxy = std::make_shared<LayoutAnimationDelegateProxy>((__bridge void *)self);
      _animationDriver = std::make_shared<LayoutAnimationDriver>(
          toolbox.runtimeExecutor, toolbox.contextContainer, _layoutAnimationDelegateProxy.get());

      _uiRunLoopObserver = std::make_unique<MainRunLoopObserver>(
          RunLoopObserver::Activity::BeforeWaiting, _layoutAnimationDelegateProxy);

      _uiRunLoopObserver->setDelegate(_layoutAnimationDelegateProxy.get());
    }

    if (ReactNativeFeatureFlags::useSharedAnimatedBackend()) {
      toolbox.animationChoreographer = std::make_shared<RCTAnimationChoreographer>();
    }

    _scheduler = std::make_unique<Scheduler>(
        toolbox, (_animationDriver ? _animationDriver.get() : nullptr), _delegateProxy.get());
  }

  return self;
}

- (void)animationTick
{
  _scheduler->animationTick();
}

- (void)reportMount:(facebook::react::SurfaceId)surfaceId
{
  _scheduler->reportMount(surfaceId);
}

- (void)dealloc
{
#if LOOM_WORKER
  LoomBinding::shutdownWorkerForUIManager(*_scheduler->getUIManager());
#endif
  if (_animationDriver) {
    _animationDriver->setLayoutAnimationStatusDelegate(nullptr);
  }

  _scheduler->setDelegate(nullptr);
}

- (void)registerSurface:(const facebook::react::SurfaceHandler &)surfaceHandler
{
  _scheduler->registerSurface(surfaceHandler);
}

- (void)unregisterSurface:(const facebook::react::SurfaceHandler &)surfaceHandler
{
  _scheduler->unregisterSurface(surfaceHandler);
}

- (const ComponentDescriptor *)findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN:(ComponentHandle)handle
{
  return _scheduler->findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN(handle);
}

- (EventDispatcher::Shared)directRendererEventDispatcher
{
  return _scheduler->getEventDispatcher();
}

- (SharedComponentDescriptorRegistry)directRendererComponentDescriptorRegistry
{
  return _scheduler->getComponentDescriptorRegistry();
}

- (void)setupAnimationDriver:(const facebook::react::SurfaceHandler &)surfaceHandler
{
  surfaceHandler.getMountingCoordinator()->setMountingOverrideDelegate(_animationDriver);
}

- (void)onAnimationStarted
{
  if (_uiRunLoopObserver) {
    _uiRunLoopObserver->enable();
  }
}

- (void)onAllAnimationsComplete
{
  if (_uiRunLoopObserver) {
    _uiRunLoopObserver->disable();
  }
}

- (void)addEventListener:(const std::shared_ptr<EventListener> &)listener
{
  return _scheduler->addEventListener(listener);
}

- (void)removeEventListener:(const std::shared_ptr<EventListener> &)listener
{
  return _scheduler->removeEventListener(listener);
}

- (const std::shared_ptr<facebook::react::UIManager>)uiManager
{
  return _scheduler->getUIManager();
}

@end
