/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#import "RCTSurfacePresenter.h"
#import <React/RCTFont.h>

#import <mutex>
#import <cmath>
#import <limits>
#import <shared_mutex>
#import <unordered_set>

#import <React/RCTAssert.h>
#import <React/RCTBridge+Private.h>
#import <React/RCTComponentViewFactory.h>
#import <React/RCTComponentViewRegistry.h>
#import <React/RCTConstants.h>
#import <React/RCTFabricSurface.h>
#import <React/RCTI18nUtil.h>
#import <React/RCTLog.h>
#import <React/RCTMountingManager.h>
#import <React/RCTMountingManagerDelegate.h>
#import <React/RCTMountingTransactionObserving.h>
#import <React/RCTScheduler.h>
#import <React/RCTSurfaceRegistry.h>
#import <React/RCTSurfaceView+Internal.h>
#import <React/RCTSurfaceView.h>
#import <React/RCTComponent.h>
#import <React/RCTUtils.h>
#import <react/utils/FollyConvert.h>

#import <react/featureflags/ReactNativeFeatureFlags.h>
#import <react/renderer/animationbackend/AnimatedProps.h>
#import <react/renderer/componentregistry/ComponentDescriptorFactory.h>
#import <react/renderer/components/image/ImageComponentDescriptor.h>
#import <react/renderer/components/image/ImageShadowNode.h>
#import <react/renderer/components/image/ImageState.h>
#import <react/renderer/components/scrollview/ScrollViewShadowNode.h>
#import <react/renderer/components/scrollview/ScrollViewState.h>
#import <react/renderer/components/text/BaseTextProps.h>
#import <react/renderer/components/text/ParagraphComponentDescriptor.h>
#import <react/renderer/components/text/ParagraphEventEmitter.h>
#import <react/renderer/components/text/ParagraphShadowNode.h>
#import <react/renderer/components/text/TextComponentDescriptor.h>
#import <react/renderer/components/text/TextProps.h>
#import <react/renderer/components/view/ViewEventEmitter.h>
#import <react/renderer/core/DynamicEventPayload.h>
#import <react/renderer/core/ShadowNodeFamily.h>
#import <react/renderer/mounting/MountingTransaction.h>
#import <react/renderer/textlayoutmanager/TextLayoutContext.h>
#import <react/renderer/textlayoutmanager/TextLayoutManager.h>
#import <react/renderer/runtimescheduler/RuntimeScheduler.h>
#import <react/renderer/scheduler/SchedulerToolbox.h>
#import <react/utils/ContextContainer.h>
#import <react/utils/ManagedObjectWrapper.h>
#import <react/renderer/componentregistry/componentNameByReactViewName.h>
#import <react/renderer/uimanager/LoomMountBatch.h>
#import <react/renderer/core/RawPropsParser.h>
#import <react/renderer/imagemanager/ImageManager.h>
#import <react/renderer/imagemanager/ImageRequestParams.h>
#import <react/renderer/imagemanager/primitives.h>
#if LOOM_RENDERER
#import <react/renderer/uimanager/LoomBinding.h>
#import <react/renderer/uimanager/LoomEventManager.h>
#import <react/renderer/uimanager/LoomRenderer.h>
#import <react/renderer/uimanager/LoomHost.h>
#import <react/renderer/uimanager/LoomTextLayoutManagerServices.h>
#import <react/renderer/uimanager/LoomViewManagerNames.h>
#import <dispatch/dispatch.h>
#import <os/log.h>
#import <pthread.h>
#endif
#import "AppleEventBeat.h"

#import "PlatformRunLoopObserver.h"
#import "RCTConversions.h"

using namespace facebook;
using namespace facebook::react;

@interface RCTSurfacePresenter () <RCTSchedulerDelegate, RCTMountingManagerDelegate>
- (void)_drainPendingReactRevisionMerges;
#if LOOM_RENDERER
// What the Loom host (LoomIOSHost, below) forwards to.
- (BOOL)schedulerDirectRendererCanMeasureTextOnWorker;
- (void)schedulerDirectRendererApplyBatch:(SurfaceId)surfaceId batch:(LoomMountBatch)batch;
- (void)schedulerDirectRendererFailSurface:(SurfaceId)surfaceId reason:(const std::string &)reason;
- (void)schedulerDirectRendererRegisterEventTarget:(SurfaceId)surfaceId
                                               tag:(Tag)tag
                                    instanceHandle:(InstanceHandle::Shared)instanceHandle;
- (void)schedulerDirectRendererUnregisterEventTarget:(SurfaceId)surfaceId tag:(Tag)tag;
- (void)schedulerDirectRendererDispatchEvent:(SurfaceId)surfaceId
                                         tag:(Tag)tag
                                        name:(const std::string &)name
                                     payload:(folly::dynamic)payload
                                    category:(RawEvent::Category)category
                                      unique:(BOOL)unique;
- (std::optional<facebook::react::Size>)schedulerDirectRendererMeasureText:(SurfaceId)surfaceId
                                                          attributedString:(const AttributedString &)attributedString
                                                       paragraphAttributes:(const ParagraphAttributes &)paragraphAttributes
                                                               minimumSize:(const facebook::react::Size &)minimumSize
                                                               maximumSize:(const facebook::react::Size &)maximumSize;
- (LoomTextMeasurement)schedulerDirectRendererPrepareText:(SurfaceId)surfaceId
                                         attributedString:(const AttributedString &)attributedString
                                      paragraphAttributes:(const ParagraphAttributes &)paragraphAttributes
                                              minimumSize:(const facebook::react::Size &)minimumSize
                                              maximumSize:(const facebook::react::Size &)maximumSize;
- (LoomTextMeasurement)schedulerDirectRendererPrepareTextForMount:(SurfaceId)surfaceId
                                                 attributedString:(const AttributedString &)attributedString
                                              paragraphAttributes:(const ParagraphAttributes &)paragraphAttributes
                                                      minimumSize:(const facebook::react::Size &)minimumSize
                                                      maximumSize:(const facebook::react::Size &)maximumSize;
- (void)schedulerDirectRendererUpdateTextState:(const LoomTextStateUpdate &)update;
- (void)schedulerDirectRendererUpdateComponentState:(SurfaceId)surfaceId
                                                tag:(Tag)tag
                                              state:(const State::Shared &)state;
- (void)schedulerDirectRendererUpdateScrollContentBounds:(SurfaceId)surfaceId
                                                     tag:(Tag)tag
                                                   props:(const folly::dynamic &)props
                                                  bounds:(const facebook::react::Rect &)bounds;
- (void)schedulerDirectRendererDispatchCommand:(SurfaceId)surfaceId
                                           tag:(Tag)tag
                                          name:(const std::string &)name
                                          args:(const folly::dynamic &)args;
- (void)schedulerDirectRendererAccessibilityEvent:(SurfaceId)surfaceId tag:(Tag)tag name:(const std::string &)name;
/** Origin of `tag`'s view in its surface's root view, on the main thread. NO if it is not mounted. */
- (BOOL)schedulerDirectRendererViewLocationInSurface:(SurfaceId)surfaceId tag:(Tag)tag location:(CGPoint *)location;
- (void)schedulerDirectRendererSetJSResponder:(SurfaceId)surfaceId
                                          tag:(Tag)tag
                                  isResponder:(BOOL)isResponder
                         blockNativeResponder:(BOOL)blockNativeResponder;
- (float)loomFontSizeMultiplier;
#endif
@end

namespace {
class ReactRevisionMergeRunLoopObserverDelegate final : public RunLoopObserver::Delegate {
 public:
  explicit ReactRevisionMergeRunLoopObserverDelegate(RCTSurfacePresenter *surfacePresenter)
      : surfacePresenter_(surfacePresenter)
  {
  }

  void activityDidChange(const RunLoopObserver::Delegate * /*delegate*/, RunLoopObserver::Activity /*activity*/)
      const noexcept override
  {
    [surfacePresenter_ _drainPendingReactRevisionMerges];
  }

 private:
  __weak RCTSurfacePresenter *surfacePresenter_;
};
} // namespace


#if LOOM_RENDERER
// Resolves the runtime settings (LoomSettings.h) before any surface starts: the built-in
// defaults, then the Info.plist `Loom` dictionary (key = the setting's name, e.g. `asyncText`),
// then the legacy overrides: NSUserDefaults (so launch arguments such as `-LoomAsyncText NO`)
// and the LOOM_PROFILE_WORKER environment variable. JavaScript writes (`Loom.x = y`) come after.
#ifndef LOOM_IOS_ASYNC_TEXT_FIRST_DRAW
#define LOOM_IOS_ASYNC_TEXT_FIRST_DRAW 1
#endif

// NSString "NO"/"YES" (launch arguments) and NSNumber both answer boolValue.
static int LoomSettingNumber(id value, facebook::react::LoomSettingType type, bool *found)
{
  const bool isBool = type == facebook::react::LoomSettingType::Bool;
  *found = [value respondsToSelector:isBool ? @selector(boolValue) : @selector(intValue)];
  if (!*found) {
    return 0;
  }
  return isBool ? ([value boolValue] ? 1 : 0) : [value intValue];
}

static void LoomIOSLoadSettings()
{
  using namespace facebook::react;
  LoomSettings::configurePlatform(kLoomPlatformIOS);
  LoomSettings::set(LoomSettingId::AsyncTextFirstDraw, LOOM_IOS_ASYNC_TEXT_FIRST_DRAW != 0);
  NSDictionary *config = [NSBundle.mainBundle objectForInfoDictionaryKey:@"Loom"];
  if (![config isKindOfClass:NSDictionary.class]) {
    config = nil;
  }
  for (size_t i = 0; i < LoomSettings::kCount; i++) {
    const auto setting = static_cast<LoomSettingId>(i);
    const auto &info = LoomSettings::info(setting);
    if (info.readOnly || !LoomSettings::applies(setting)) {
      continue;
    }
    bool found = false;
    int number = LoomSettingNumber(config[@(info.name)], info.type, &found);
    if (found) {
      LoomSettings::set(setting, number);
    }
    if (info.iosKey != nullptr) {
      number = LoomSettingNumber([NSUserDefaults.standardUserDefaults objectForKey:@(info.iosKey)], info.type, &found);
      if (found) {
        LoomSettings::set(setting, number);
      }
    }
  }
  const char *profile = getenv("LOOM_PROFILE_WORKER");
  if (profile != nullptr && profile[0] == '1') {
    LoomSettings::set(LoomSettingId::ProfileWorker, 1);
  }
}

// Applies an update as `cloneProps(oldProps, rawProps)`, which keeps every key the raw
// props leave out (what Fabric does with React's diff), so every component can take only
// the changed keys. Image is the exception: its state is rebuilt from the raw props the
// applier was given.
static bool LoomSendsPropDiffs(const std::string &hostName)
{
  return hostName.find("Image") == std::string::npos;
}

static void LoomLog(LoomLogLevel level, const char *tag, const char *message)
{
  os_log(OS_LOG_DEFAULT, "%{public}s: %{public}s", tag, message);
  if (level != LoomLogLevel::Info) {
    // The bench harness reads the device console, which has stderr only.
    fprintf(stderr, "%s: %s\n", tag, message);
  }
}

static LoomPlatformTraits LoomIOSTraits()
{
  LoomPlatformTraits traits;
  traits.components = loomViewManagerComponentTable();
  traits.sendsPropDiffs = LoomSendsPropDiffs;
  // Paragraph views lay their text out in the state's content size.
  traits.republishTextStateOnResize = true;
  // ParagraphComponentView draws attributed-string state through TextLayoutManager; the
  // measurement API has no renderable prepared-layout handle to publish.
  traits.hasPreparedTextLayout = false;
  traits.parallelFor = [](size_t count, const std::function<void(size_t)> &body) {
    const auto *work = &body;
    dispatch_apply(count, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t index) {
      (*work)(index);
    });
  };
  traits.log = LoomLog;
  traits.isUIThread = [] { return pthread_main_np() != 0; };
  // std::thread's 512 KB default is too small for Yoga plus the worker's recursive tree
  // walks; the stack is at least as large as the JS thread's, which React Native doubles
  // under RCT_DEBUG.
  traits.workerStackSize = 2 * 1024 * 1024;
  traits.workerThreadName = "com.loom.worker";
  traits.setCurrentThreadName = [](const char *name) { pthread_setname_np(name); };
  return traits;
}

namespace {
/** The presenter's text engine: its measurement and prepared-layout paths, and RN's own attribute parser. */
class LoomIOSTextServices final : public LoomTextLayoutManagerServices {
 public:
  LoomIOSTextServices(std::shared_ptr<const ContextContainer> contextContainer, RCTSurfacePresenter *presenter)
      : LoomTextLayoutManagerServices(std::move(contextContainer), nullptr, false), presenter_(presenter)
  {
  }

  std::optional<facebook::react::Size> measureText(
      SurfaceId surfaceId,
      const AttributedString &attributedString,
      const ParagraphAttributes &paragraphAttributes,
      const facebook::react::Size &minimumSize,
      const facebook::react::Size &maximumSize) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return std::nullopt;
    }
    return [presenter schedulerDirectRendererMeasureText:surfaceId
                                         attributedString:attributedString
                                      paragraphAttributes:paragraphAttributes
                                              minimumSize:minimumSize
                                              maximumSize:maximumSize];
  }

  bool canMeasureOnWorker() override
  {
    RCTSurfacePresenter *presenter = presenter_;
    return presenter != nil && [presenter schedulerDirectRendererCanMeasureTextOnWorker];
  }

  TextAttributes parseTextAttributes(
      SurfaceId surfaceId,
      const std::string &componentName,
      const folly::dynamic &props,
      const TextAttributes &inherited,
      ParagraphAttributes &outParagraphAttributes) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return inherited;
    }
    auto adjusted = inherited;
    adjusted.fontSizeMultiplier = [presenter loomFontSizeMultiplier];
    return LoomTextLayoutManagerServices::parseTextAttributes(
        surfaceId, componentName, props, adjusted, outParagraphAttributes);
  }

  LoomTextMeasurement prepareText(
      SurfaceId surfaceId,
      const AttributedString &attributedString,
      const ParagraphAttributes &paragraphAttributes,
      const facebook::react::Size &minimumSize,
      const facebook::react::Size &maximumSize) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return LoomTextMeasurement{};
    }
    return [presenter schedulerDirectRendererPrepareText:surfaceId
                                         attributedString:attributedString
                                      paragraphAttributes:paragraphAttributes
                                              minimumSize:minimumSize
                                              maximumSize:maximumSize];
  }

  LoomTextMeasurement prepareTextForMount(
      SurfaceId surfaceId,
      const AttributedString &attributedString,
      const ParagraphAttributes &paragraphAttributes,
      const facebook::react::Size &minimumSize,
      const facebook::react::Size &maximumSize) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return LoomTextMeasurement{};
    }
    return [presenter schedulerDirectRendererPrepareTextForMount:surfaceId
                                                 attributedString:attributedString
                                              paragraphAttributes:paragraphAttributes
                                                      minimumSize:minimumSize
                                                      maximumSize:maximumSize];
  }

 private:
  __weak RCTSurfacePresenter *presenter_;
};

/** The iOS `LoomHost`: the main queue is the UI thread, and the presenter owns the views. */
class LoomIOSHost final : public LoomHost {
 public:
  LoomIOSHost(RCTSurfacePresenter *presenter, std::shared_ptr<const ContextContainer> contextContainer)
      : presenter_(presenter), text_(std::move(contextContainer), presenter)
  {
  }

  LoomPlatformTraits traits() const override
  {
    return LoomIOSTraits();
  }

  std::function<void(std::function<void()>)> uiExecutor() override
  {
    return [](std::function<void()> callback) {
      dispatch_async(dispatch_get_main_queue(), ^{ callback(); });
    };
  }

  void applyBatch(SurfaceId surfaceId, LoomMountBatch batch) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererApplyBatch:surfaceId batch:std::move(batch)];
  }

  void failSurface(SurfaceId surfaceId, const std::string &reason) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererFailSurface:surfaceId reason:reason];
  }

  void updateViewProps(Tag tag, const folly::dynamic &props) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDidSynchronouslyUpdateViewOnUIThread:tag props:props];
  }

  void registerEventTarget(SurfaceId surfaceId, Tag tag, InstanceHandle::Shared instanceHandle) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererRegisterEventTarget:surfaceId tag:tag instanceHandle:std::move(instanceHandle)];
  }

  void unregisterEventTarget(SurfaceId surfaceId, Tag tag) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererUnregisterEventTarget:surfaceId tag:tag];
  }

  void dispatchEvent(
      SurfaceId surfaceId,
      Tag tag,
      const std::string &eventName,
      folly::dynamic payload,
      RawEvent::Category category,
      bool unique) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererDispatchEvent:surfaceId
                                                 tag:tag
                                                name:eventName
                                             payload:std::move(payload)
                                            category:category
                                              unique:unique];
  }

  void updateComponentState(SurfaceId surfaceId, Tag tag, const std::shared_ptr<const State> &state) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererUpdateComponentState:surfaceId tag:tag state:state];
  }

  void updateTextState(const LoomTextStateUpdate &update) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererUpdateTextState:update];
  }

  void updateScrollContentBounds(
      SurfaceId surfaceId,
      Tag tag,
      const folly::dynamic &props,
      const facebook::react::Rect &bounds) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererUpdateScrollContentBounds:surfaceId tag:tag props:props bounds:bounds];
  }

  void dispatchCommand(SurfaceId surfaceId, Tag tag, const std::string &commandName, const folly::dynamic &args)
      override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererDispatchCommand:surfaceId tag:tag name:commandName args:args];
  }

  void sendAccessibilityEvent(SurfaceId surfaceId, Tag tag, const std::string &eventType) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererAccessibilityEvent:surfaceId tag:tag name:eventType];
  }

  void setIsJSResponder(SurfaceId surfaceId, Tag tag, bool isJSResponder, bool blockNativeResponder) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return;
    }
    [presenter schedulerDirectRendererSetJSResponder:surfaceId
                                                  tag:tag
                                          isResponder:isJSResponder
                                 blockNativeResponder:blockNativeResponder];
  }

  std::optional<std::pair<float, float>> getViewLocationInSurface(SurfaceId surfaceId, Tag tag) override
  {
    RCTSurfacePresenter *presenter = presenter_;
    if (!presenter) {
      return std::nullopt;
    }
    CGPoint location;
    if (![presenter schedulerDirectRendererViewLocationInSurface:surfaceId tag:tag location:&location]) {
      return std::nullopt;
    }
    return std::pair<float, float>{static_cast<float>(location.x), static_cast<float>(location.y)};
  }

  LoomTextServices *textServices() override
  {
    return &text_;
  }

 private:
  __weak RCTSurfacePresenter *presenter_;
  LoomIOSTextServices text_;
};
} // namespace
#endif

extern "C" void LoomTextSetApplyingAnimationFrame(bool animationFrame);

@implementation RCTSurfacePresenter {
  RCTMountingManager *_mountingManager; // Thread-safe.
  RCTSurfaceRegistry *_surfaceRegistry; // Thread-safe.

  std::mutex _schedulerAccessMutex;
  std::mutex _schedulerLifeCycleMutex;
  RCTScheduler *_Nullable _scheduler; // Thread-safe. Pointer is protected by `_schedulerAccessMutex`.
  std::shared_ptr<const ContextContainer> _contextContainer; // Protected by `_schedulerLifeCycleMutex`.
  RuntimeExecutor _runtimeExecutor; // Protected by `_schedulerLifeCycleMutex`.
  std::optional<RuntimeExecutor> _bridgelessBindingsExecutor; // Only used for installing bindings.

  std::shared_mutex _observerListMutex;
  std::vector<__weak id<RCTSurfacePresenterObserver>> _observers; // Protected by `_observerListMutex`.

  std::mutex _pendingReactRevisionMergesMutex;
  // Pending React revision merges, drained on the main run loop just before it sleeps.
  std::unordered_set<SurfaceId> _pendingReactRevisionMerges; // Protected by `_pendingReactRevisionMergesMutex`.
  std::shared_ptr<ReactRevisionMergeRunLoopObserverDelegate> _mergeRunLoopObserverDelegate;
  std::unique_ptr<const MainRunLoopObserver> _mergeRunLoopObserver;
#if LOOM_RENDERER
  std::shared_ptr<const TextLayoutManager> _directTextLayoutManager;
  std::atomic<float> _directTextScale;
  std::atomic<float> _directFontSizeMultiplier;
  std::shared_ptr<ImageManager> _directImageManager;
  // Targets live here rather than in the component-view registry so that
  // view-less (flattened) tags still receive events such as onLayout. The
  // family map below only keeps alive the typed emitter installed on the
  // component views that actually exist.
  std::shared_ptr<LoomEventManager> _directEventManager;
  std::unordered_map<Tag, ShadowNodeFamily::Shared> _directEventFamilies;
  std::unordered_map<Tag, InstanceHandle::Shared> _directInstanceHandles;
  std::unordered_map<Tag, std::string> _directComponentNames;
  std::unordered_map<Tag, SurfaceId> _directSurfaceByTag;
  std::unordered_set<Tag> _directViewlessTags;
  std::unordered_set<Tag> _directReplacedTags;
  std::unordered_map<Tag, State::Shared> _directPendingStates;
  // The last paragraph state delivered per tag, with what it was built from. A republish whose
  // string (by identity), paragraph attributes and layout size are unchanged reuses it instead of
  // copying the attributed string into a new ParagraphState.
  struct DirectTextState {
    std::shared_ptr<const AttributedString> attributedString;
    ParagraphAttributes paragraphAttributes;
    facebook::react::Size layoutSize;
    ParagraphShadowNode::ConcreteState::Shared state;
  };
  std::unordered_map<Tag, DirectTextState> _directTextStates;
  std::unordered_map<Tag, folly::dynamic> _directImageProps;
  std::unordered_map<Tag, ImageShadowNode::ConcreteState::Shared> _directImageStates;
  std::unordered_map<Tag, ScrollViewShadowNode::ConcreteState::Shared> _directScrollStates;
  // Fabric compatibility layer: the last state object of each compat node's own ShadowNode.
  std::unordered_map<Tag, State::Shared> _directCompatStates;
  std::unordered_map<Tag, LayoutMetrics> _directLayoutMetrics;
  std::unordered_map<Tag, std::array<float, 4>> _directContentInsets;
  std::unordered_map<Tag, std::array<float, 4>> _directOverflowInsets;
  // Views updated by the commit being mounted, and what changed in each: like Fabric's mounting
  // manager, a view gets one finalizeUpdates: with everything applied, after the commit's last
  // operation, instead of one per operation (a commit delivers text states before the batch that
  // carries their layout).
  NSMutableArray<UIView<RCTComponentViewProtocol> *> *_directFinalizeViews;
  std::unordered_map<const void *, RNComponentViewUpdateMask> _directFinalizeMasks;
  // Mounted views that implement RCTMountingTransactionObserving, per surface. Like Fabric's
  // mounting manager, each batch is announced to them before and after it is applied, described
  // as ShadowViewMutations. Surfaces without such a view never build that description.
  std::unordered_map<SurfaceId, std::unordered_map<Tag, __weak UIView<RCTComponentViewProtocol> *>>
      _directTransactionObservers;
  MountingTransaction::Number _directTransactionNumber;
#endif
}

#if LOOM_RENDERER
- (EventEmitter::Shared)directEventEmitterForSurface:(SurfaceId)surfaceId
                                                 tag:(Tag)tag
                                 componentDescriptor:(const ComponentDescriptor &)componentDescriptor
                                      instanceHandle:(InstanceHandle::Shared)instanceHandle
{
  auto family = componentDescriptor.createFamily(
      ShadowNodeFamilyFragment{tag, surfaceId, std::move(instanceHandle)});
  auto eventEmitter = family->getEventEmitter();
  eventEmitter->getEventTarget()->setEnabled(true);
  _directEventFamilies[tag] = std::move(family);
  return eventEmitter;
}
#endif

- (instancetype)initWithContextContainer:(std::shared_ptr<const ContextContainer>)contextContainer
                         runtimeExecutor:(RuntimeExecutor)runtimeExecutor
              bridgelessBindingsExecutor:(std::optional<RuntimeExecutor>)bridgelessBindingsExecutor
{
  if (self = [super init]) {
    assert(contextContainer && "RuntimeExecutor must be not null.");
    _runtimeExecutor = runtimeExecutor;
    _bridgelessBindingsExecutor = bridgelessBindingsExecutor;
    _contextContainer = contextContainer;
#if LOOM_RENDERER
    // Published once. TextKit allocates storage/layout/container per measure;
    // its attributed-string and C++ measure caches are synchronized.
    _directTextScale.store(1.0f);
    _directFontSizeMultiplier.store(1.0f);
    _directTextLayoutManager = std::make_shared<TextLayoutManager>(_contextContainer);
    LoomIOSLoadSettings();
    LoomHost::registerIn(*_contextContainer, std::make_shared<LoomIOSHost>(self, _contextContainer));
#endif

    _surfaceRegistry = [RCTSurfaceRegistry new];
    _mountingManager = [RCTMountingManager new];
    _mountingManager.contextContainer = contextContainer;
    _mountingManager.delegate = self;

    if (ReactNativeFeatureFlags::enableFabricCommitBranchingMergeOnMainThread()) {
      _mergeRunLoopObserverDelegate = std::make_shared<ReactRevisionMergeRunLoopObserverDelegate>(self);
      _mergeRunLoopObserver = std::make_unique<const MainRunLoopObserver>(
          RunLoopObserver::Activity::BeforeWaiting, _mergeRunLoopObserverDelegate);
      _mergeRunLoopObserver->setDelegate(_mergeRunLoopObserverDelegate.get());
      _mergeRunLoopObserver->enable();
    }

    _scheduler = [self _createScheduler];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(_applicationWillTerminate)
                                                 name:UIApplicationWillTerminateNotification
                                               object:nil];
  }

  return self;
}

- (RCTMountingManager *)mountingManager
{
  return _mountingManager;
}

- (RCTScheduler *_Nullable)scheduler
{
  std::lock_guard<std::mutex> lock(_schedulerAccessMutex);
  return _scheduler;
}

- (std::shared_ptr<const ContextContainer>)contextContainer
{
  std::lock_guard<std::mutex> lock(_schedulerLifeCycleMutex);
  return _contextContainer;
}

- (void)setRuntimeExecutor:(RuntimeExecutor)runtimeExecutor
{
  std::lock_guard<std::mutex> lock(_schedulerLifeCycleMutex);
  _runtimeExecutor = runtimeExecutor;
}

#pragma mark - Internal Surface-dedicated Interface

- (void)registerSurface:(RCTFabricSurface *)surface
{
  [_surfaceRegistry registerSurface:surface];
  RCTScheduler *scheduler = [self scheduler];
  if (scheduler) {
    [scheduler registerSurface:surface.surfaceHandler];
  }
}

- (void)unregisterSurface:(RCTFabricSurface *)surface
{
#if LOOM_RENDERER
  const auto surfaceId = surface.surfaceHandler.getSurfaceId();
  dispatch_block_t releaseDirectViews = ^{
    RCTAssertMainQueue();
    auto registry = self->_mountingManager.componentViewRegistry;
    std::vector<Tag> tags;
    for (const auto &[tag, ownerSurface] : self->_directSurfaceByTag) {
      if (ownerSurface == surfaceId) {
        tags.push_back(tag);
      }
    }
    for (const auto tag : tags) {
      if (auto *view = [registry findComponentViewWithTag:tag]) {
        [view removeFromSuperview];
      }
    }
    for (const auto tag : tags) {
      if (self->_directViewlessTags.erase(tag) == 0) {
        if (auto *view = [registry findComponentViewWithTag:tag]) {
          auto descriptor = [registry componentViewDescriptorWithTag:tag];
          auto handle = [descriptor.view.class componentDescriptorProvider].handle;
          [registry enqueueComponentViewWithComponentHandle:handle
                                                        tag:tag
                                    componentViewDescriptor:descriptor];
        }
      }
      if (self->_directEventManager) {
        self->_directEventManager->unregisterTarget(surfaceId, tag);
      }
      self->_directEventFamilies.erase(tag);
      self->_directInstanceHandles.erase(tag);
      self->_directReplacedTags.erase(tag);
      self->_directPendingStates.erase(tag);
      self->_directImageProps.erase(tag);
      self->_directImageStates.erase(tag);
      self->_directScrollStates.erase(tag);
      self->_directTextStates.erase(tag);
      self->_directCompatStates.erase(tag);
      self->_directComponentNames.erase(tag);
      self->_directLayoutMetrics.erase(tag);
      self->_directContentInsets.erase(tag);
      self->_directOverflowInsets.erase(tag);
      self->_directSurfaceByTag.erase(tag);
    }
    self->_directTransactionObservers.erase(surfaceId);
    LoomBinding::releaseSurface(surfaceId);
  };
  if (RCTIsMainQueue()) {
    releaseDirectViews();
  } else {
    dispatch_sync(dispatch_get_main_queue(), releaseDirectViews);
  }
#endif
  RCTScheduler *scheduler = [self scheduler];
  if (scheduler) {
    [scheduler unregisterSurface:surface.surfaceHandler];
  }
  [_surfaceRegistry unregisterSurface:surface];
}

- (RCTFabricSurface *)surfaceForRootTag:(ReactTag)rootTag
{
  return [_surfaceRegistry surfaceForRootTag:rootTag];
}

- (id<RCTSurfaceProtocol>)createFabricSurfaceForModuleName:(NSString *)moduleName
                                         initialProperties:(NSDictionary *)initialProperties
{
  return [[RCTFabricSurface alloc] initWithSurfacePresenter:self
                                                 moduleName:moduleName
                                          initialProperties:initialProperties];
}

- (UIView *)findComponentViewWithTag_DO_NOT_USE_DEPRECATED:(NSInteger)tag
{
  UIView<RCTComponentViewProtocol> *componentView =
      [_mountingManager.componentViewRegistry findComponentViewWithTag:tag];
  return componentView;
}

- (void)synchronouslyUpdateViewOnUIThread:(NSNumber *)reactTag props:(NSDictionary *)props
{
  ReactTag tag = [reactTag integerValue];
  [self schedulerDidSynchronouslyUpdateViewOnUIThread:tag props:convertIdToFollyDynamic(props)];
}

- (void)schedulerDidSynchronouslyUpdateViewOnUIThread:(ReactTag)tag props:(folly::dynamic)props
{
#if LOOM_WORKER
  LoomBinding::recordNativeProps(tag, props);
#endif
  RCTScheduler *scheduler = [self scheduler];
  if (!scheduler) {
    return;
  }

  UIView<RCTComponentViewProtocol> *componentView =
      [_mountingManager.componentViewRegistry findComponentViewWithTag:tag];
  if (componentView == nil) {
    return; // This view probably isn't managed by Fabric
  }
  ComponentHandle handle = [[componentView class] componentDescriptorProvider].handle;
  auto *componentDescriptor = [scheduler findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN:handle];

  if (!componentDescriptor) {
    return;
  }

  [_mountingManager synchronouslyUpdateViewOnUIThread:tag
                                         changedProps:std::move(props)
                                  componentDescriptor:*componentDescriptor];
}

#if LOOM_RENDERER
- (void)schedulerDidSynchronouslyUpdateAnimatedProps:
    (const std::unordered_map<facebook::react::Tag, facebook::react::AnimatedProps> &)updates
{
  // Loom's animation lane already recorded these frames; this only writes them
  // to the views. Each view's current props are copied and the animated fields
  // set in place, which costs no dictionary and no parse.
  RCTAssertMainQueue();
  RCTScheduler *scheduler = [self scheduler];
  if (!scheduler || !_contextContainer) {
    return;
  }
  auto *registry = _mountingManager.componentViewRegistry;
  for (const auto &[tag, animated] : updates) {
    UIView<RCTComponentViewProtocol> *view = [registry findComponentViewWithTag:tag];
    if (view == nil) {
      continue;
    }
    const auto handle = [[view class] componentDescriptorProvider].handle;
    const auto *descriptor = [scheduler findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN:handle];
    if (descriptor == nullptr) {
      continue;
    }
    const auto oldProps = [view props];
    SurfaceId surfaceId = -1;
    for (UIView *ancestor = view; ancestor != nil; ancestor = ancestor.superview) {
      if (RCTIsReactRootView(@(ancestor.tag))) {
        surfaceId = static_cast<SurfaceId>(ancestor.tag);
        break;
      }
    }
    const PropsParserContext context{surfaceId, *_contextContainer};
    auto newProps = animated.rawProps
        ? descriptor->cloneProps(context, oldProps, RawProps(*animated.rawProps))
        : descriptor->cloneProps(context, oldProps, RawProps(folly::dynamic::object()));
    if (auto viewProps = std::dynamic_pointer_cast<const BaseViewProps>(newProps)) {
      auto &mutableProps = const_cast<BaseViewProps &>(*viewProps);
      for (const auto &animatedProp : animated.props) {
        cloneProp(mutableProps, *animatedProp);
      }
    }
    // A view skips the keys an earlier synchronous update marked as managed by
    // Animated; like that update, this one is the animation writing them.
    NSSet<NSString *> *managedKeys = view.propKeysManagedByAnimated_DO_NOT_USE_THIS_IS_BROKEN;
    view.propKeysManagedByAnimated_DO_NOT_USE_THIS_IS_BROKEN = nil;
    [view updateProps:newProps oldProps:oldProps];
    view.propKeysManagedByAnimated_DO_NOT_USE_THIS_IS_BROKEN = managedKeys;
    [view finalizeUpdates:RNComponentViewUpdateMaskProps];
  }
}

- (void)directDeferFinalize:(UIView<RCTComponentViewProtocol> *)view mask:(RNComponentViewUpdateMask)mask
{
  if (!_directFinalizeViews) {
    _directFinalizeViews = [NSMutableArray new];
  }
  auto [entry, inserted] = _directFinalizeMasks.try_emplace((__bridge const void *)view, mask);
  if (inserted) {
    [_directFinalizeViews addObject:view];
    if (_directFinalizeViews.count == 1) {
      // A commit's operations end with its batch, which finalizes them; this only covers updates
      // that arrive without one.
      __weak RCTSurfacePresenter *weakSelf = self;
      dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf directFinalizeUpdates];
      });
    }
  } else {
    entry->second = entry->second | mask;
  }
}

// A view that is recycled before the commit finalizes it is not finalized.
- (void)directDropFinalize:(UIView<RCTComponentViewProtocol> *)view
{
  if (_directFinalizeMasks.erase((__bridge const void *)view) != 0) {
    [_directFinalizeViews removeObjectIdenticalTo:view];
  }
}

- (void)directFinalizeUpdates
{
  if (_directFinalizeViews.count == 0) {
    return;
  }
  NSArray<UIView<RCTComponentViewProtocol> *> *views = _directFinalizeViews;
  auto masks = std::move(_directFinalizeMasks);
  _directFinalizeViews = [NSMutableArray new];
  _directFinalizeMasks.clear();
  for (UIView<RCTComponentViewProtocol> *view in views) {
    [view finalizeUpdates:masks[(__bridge const void *)view]];
  }
}

- (void)schedulerDirectRendererApplyBatch:(SurfaceId)surfaceId batch:(LoomMountBatch)batch
{
  RCTAssertMainQueue();
  // Text resized by an animation frame is drawn in this frame (RCTParagraphComponentView).
  LoomTextSetApplyingAnimationFrame(batch.animationFrame);
  RCTScheduler *scheduler = [self scheduler];
  if (!scheduler) {
    return;
  }
  auto descriptors = scheduler.directRendererComponentDescriptorRegistry;
  if (!descriptors) {
    return;
  }

  auto registry = _mountingManager.componentViewRegistry;
  auto context = PropsParserContext{surfaceId, *_contextContainer};
  [_mountingManager.delegate mountingManager:_mountingManager willMountComponentsWithRootTag:surfaceId];
  // Built at most once per batch, and only when the surface has a transaction observer.
  std::optional<MountingTransaction> observedTransaction;
  auto transactionForObservers = [&]() -> const MountingTransaction & {
    if (!observedTransaction) {
      observedTransaction.emplace(
          surfaceId, ++_directTransactionNumber, [self directMutationsForBatch:batch descriptors:*descriptors],
          TransactionTelemetry{});
    }
    return *observedTransaction;
  };
  [self directNotifyTransactionObservers:surfaceId
                                  before:YES
                             transaction:transactionForObservers];
  auto updateImageState = [&](Tag tag, const folly::dynamic &rawProps) {
    const auto componentName = _directComponentNames.find(tag);
    const auto layoutMetrics = _directLayoutMetrics.find(tag);
    auto *view = [registry findComponentViewWithTag:tag];
    if (view == nullptr || componentName == _directComponentNames.end() ||
        componentName->second != "Image" ||
        layoutMetrics == _directLayoutMetrics.end()) {
      return;
    }

    const auto &descriptor = descriptors->at("RCTImageView");
    auto props = descriptor.cloneProps(context, nullptr, RawProps(rawProps));
    const auto &imageProps = static_cast<const ImageProps &>(*props);
    const auto contentSize = layoutMetrics->second.getContentFrame().size;
    const auto scale = layoutMetrics->second.pointScaleFactor;

    ImageSource imageSource{ImageSource::Type::Invalid};
    if (imageProps.sources.size() == 1) {
      imageSource = imageProps.sources.front();
      imageSource.size = contentSize;
      imageSource.scale = scale;
    } else if (!imageProps.sources.empty()) {
      const auto targetArea = contentSize.width * contentSize.height * scale * scale;
      auto bestFit = std::numeric_limits<Float>::infinity();
      for (const auto &source : imageProps.sources) {
        const auto sourceScale = source.scale == 0 ? scale : source.scale;
        const auto sourceArea = source.size.width * source.size.height *
            sourceScale * sourceScale;
        const auto fit = std::abs(1 - sourceArea / targetArea);
        if (fit < bestFit) {
          bestFit = fit;
          imageSource = source;
        }
      }
      imageSource.size = contentSize;
      imageSource.scale = scale;
    }

    const ImageRequestParams requestParams{imageProps.blurRadius};
    const auto oldState = _directImageStates.find(tag);
    if (oldState != _directImageStates.end() &&
        oldState->second->getData().getImageSource() == imageSource &&
        oldState->second->getData().getImageRequestParams() == requestParams) {
      return;
    }

    if (!_directImageManager) {
      _directImageManager = getManagerByName<ImageManager>(
          _contextContainer, ImageManagerKey);
    }
    ImageState imageState{
        imageSource,
        _directImageManager->requestImage(
            imageSource, surfaceId, requestParams, tag),
        requestParams};
    auto stateData = std::make_shared<const ImageState>(std::move(imageState));
    auto state = std::make_shared<const ImageShadowNode::ConcreteState>(
        std::move(stateData), ShadowNodeFamily::Weak{});
    [view updateState:state oldState:nullptr];
    [self directDeferFinalize:view mask:RNComponentViewUpdateMaskState];
    _directImageStates[tag] = std::move(state);
  };
  auto applyOperation = [&](size_t operationIndex) {
    std::visit(
        [&](const auto &operation) {
          using Operation = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Operation, DirectCreateOperation>) {
            _directComponentNames[operation.tag] = componentNameByReactViewName(operation.componentName);
            _directSurfaceByTag[operation.tag] = surfaceId;
            // A create may re-use a tag that this same batch also deletes:
            // React switches a View between flattened and native (for example
            // when it gains a backgroundColor), or re-uses a tag for another
            // component after an unmount. Retire whatever the previous
            // incarnation owned, and mark the replacement so the trailing
            // delete for that incarnation does not tear down the live tag.
            auto *oldView = [registry findComponentViewWithTag:operation.tag];
            const bool wasViewless = _directViewlessTags.erase(operation.tag) != 0;
            if (oldView != nil || wasViewless) {
              _directReplacedTags.insert(operation.tag);
              _directLayoutMetrics.erase(operation.tag);
              if (oldView != nil) {
                [oldView removeFromSuperview];
                [self directDropFinalize:oldView];
                [self directUnregisterTransactionObserver:operation.tag surfaceId:surfaceId];
                auto oldDescriptor = [registry componentViewDescriptorWithTag:operation.tag];
                auto oldHandle = [oldDescriptor.view.class componentDescriptorProvider].handle;
                [registry enqueueComponentViewWithComponentHandle:oldHandle
                                                              tag:operation.tag
                                          componentViewDescriptor:oldDescriptor];
              }
            }
            if (!operation.layoutable) {
              _directViewlessTags.insert(operation.tag);
              return;
            }
            const auto &componentDescriptor = descriptors->at(operation.componentName);
            const auto &viewDescriptor = [registry dequeueComponentViewWithComponentHandle:componentDescriptor.getComponentHandle()
                                                                                         tag:operation.tag];
            [self directRegisterTransactionObserver:viewDescriptor.view surfaceId:surfaceId];
            const auto unifiedName = componentNameByReactViewName(operation.componentName);
            _directComponentNames[operation.tag] = unifiedName;
            if (unifiedName == "Image") {
              _directImageProps[operation.tag] = operation.props;
              _directImageStates.erase(operation.tag);
            }
            // A component with its own descriptor arrives with the props that descriptor parsed
            // from the JSI value; re-parsing the folly copy would lose JS-only values.
            Props::Shared props = operation.parsedProps != nullptr
                ? operation.parsedProps
                : componentDescriptor.cloneProps(context, nullptr, RawProps(operation.props));
            [viewDescriptor.view updateProps:props oldProps:nullptr];
            EventEmitter::Shared eventEmitter;
            auto instanceHandle = _directInstanceHandles.find(operation.tag);
            if (instanceHandle != _directInstanceHandles.end()) {
              eventEmitter = [self directEventEmitterForSurface:surfaceId
                                                            tag:operation.tag
                                            componentDescriptor:componentDescriptor
                                                 instanceHandle:instanceHandle->second];
              [viewDescriptor.view updateEventEmitter:eventEmitter];
            }
            [self directDeferFinalize:viewDescriptor.view
                                 mask:RNComponentViewUpdateMaskProps |
                                 (eventEmitter ? RNComponentViewUpdateMaskEventEmitter : RNComponentViewUpdateMaskNone)];
            auto pendingState = _directPendingStates.find(operation.tag);
            if (pendingState != _directPendingStates.end()) {
              [viewDescriptor.view updateState:pendingState->second oldState:nullptr];
              [self directDeferFinalize:viewDescriptor.view mask:RNComponentViewUpdateMaskState];
              _directPendingStates.erase(pendingState);
            }
          } else if constexpr (std::is_same_v<Operation, DirectUpdatePropsOperation>) {
            auto *view = [registry findComponentViewWithTag:operation.tag];
            if (!view) {
              RCTLogError(@"Loom renderer props update references missing iOS component view tag %d", operation.tag);
              return;
            }
            const auto handle = [view.class componentDescriptorProvider].handle;
            const auto *componentDescriptor = [scheduler findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN:handle];
            if (!componentDescriptor) {
              RCTLogError(@"Loom renderer has no component descriptor for iOS view tag %d", operation.tag);
              return;
            }
            auto oldProps = [view props];
            auto newProps = operation.parsedProps != nullptr
                ? operation.parsedProps
                : componentDescriptor->cloneProps(context, oldProps, RawProps(operation.props));
            [view updateProps:newProps oldProps:oldProps];
            [self directDeferFinalize:view mask:RNComponentViewUpdateMaskProps];
            if (_directComponentNames[operation.tag] == "Image") {
              _directImageProps[operation.tag] = operation.props;
              updateImageState(operation.tag, operation.props);
            }
          } else if constexpr (std::is_same_v<Operation, DirectRemoveOperation>) {
            auto *parent = [registry findComponentViewWithTag:operation.parentTag];
            auto *child = [registry findComponentViewWithTag:operation.tag];
            if (parent && child) {
              [parent unmountChildComponentView:child index:static_cast<NSInteger>(operation.index)];
            }
          } else if constexpr (std::is_same_v<Operation, DirectInsertOperation>) {
            auto *parent = [registry findComponentViewWithTag:operation.parentTag];
            auto *child = [registry findComponentViewWithTag:operation.tag];
            if (parent && child) {
              [parent mountChildComponentView:child index:static_cast<NSInteger>(operation.index)];
            } else {
              RCTLogError(@"Loom renderer insert references an unmounted iOS view (parent=%d child=%d)",
                          operation.parentTag,
                          operation.tag);
            }
          } else if constexpr (std::is_same_v<Operation, DirectPaddingOperation>) {
            _directContentInsets[operation.tag] = operation.insets;
          } else if constexpr (std::is_same_v<Operation, DirectOverflowInsetOperation>) {
            _directOverflowInsets[operation.tag] = operation.insets;
          } else if constexpr (std::is_same_v<Operation, DirectLayoutOperation>) {
            auto *view = [registry findComponentViewWithTag:operation.tag];
            if (!view) {
              return;
            }
            LayoutMetrics metrics;
            metrics.frame = {{operation.x, operation.y}, {operation.width, operation.height}};
            metrics.displayType = static_cast<DisplayType>(operation.displayType);
            metrics.layoutDirection = operation.layoutDirection == 2 ? LayoutDirection::RightToLeft
                                                                      : LayoutDirection::LeftToRight;
            metrics.pointScaleFactor = RCTScreenScale();
            if (auto insets = _directContentInsets.find(operation.tag); insets != _directContentInsets.end()) {
              metrics.contentInsets.left = insets->second[0];
              metrics.contentInsets.top = insets->second[1];
              metrics.contentInsets.right = insets->second[2];
              metrics.contentInsets.bottom = insets->second[3];
            }
            if (auto insets = _directOverflowInsets.find(operation.tag); insets != _directOverflowInsets.end()) {
              metrics.overflowInset.left = insets->second[0];
              metrics.overflowInset.top = insets->second[1];
              metrics.overflowInset.right = insets->second[2];
              metrics.overflowInset.bottom = insets->second[3];
            }
            auto oldMetrics = _directLayoutMetrics.find(operation.tag);
            [view updateLayoutMetrics:metrics
                     oldLayoutMetrics:oldMetrics == _directLayoutMetrics.end() ? EmptyLayoutMetrics : oldMetrics->second];
            [self directDeferFinalize:view mask:RNComponentViewUpdateMaskLayoutMetrics];
            _directLayoutMetrics[operation.tag] = metrics;
            if (_directComponentNames[operation.tag] == "Image") {
              auto imageProps = _directImageProps.find(operation.tag);
              if (imageProps != _directImageProps.end()) {
                updateImageState(operation.tag, imageProps->second);
              }
            }
          } else if constexpr (std::is_same_v<Operation, DirectDeleteOperation>) {
            // Check the in-batch replacement marker first: the create above
            // already retired this tag's previous incarnation, and the tag may
            // now be live again as a view-less (flattened) node whose
            // bookkeeping this delete must not erase.
            if (_directReplacedTags.erase(operation.tag) != 0) {
              return;
            }
            if (_directViewlessTags.erase(operation.tag) != 0) {
              _directComponentNames.erase(operation.tag);
              _directSurfaceByTag.erase(operation.tag);
              if (_directEventManager) {
                _directEventManager->unregisterTarget(surfaceId, operation.tag);
              }
              _directEventFamilies.erase(operation.tag);
              _directInstanceHandles.erase(operation.tag);
              _directImageProps.erase(operation.tag);
              _directImageStates.erase(operation.tag);
              _directScrollStates.erase(operation.tag);
              _directTextStates.erase(operation.tag);
              _directCompatStates.erase(operation.tag);
              // [LoomMemory] Padding and overflow-inset operations are recorded for flattened
              // tags too, but only the mounted-view branch below erased them, so they grew with
              // every flattened node ever created.
              _directPendingStates.erase(operation.tag);
              _directLayoutMetrics.erase(operation.tag);
              _directContentInsets.erase(operation.tag);
              _directOverflowInsets.erase(operation.tag);
              return;
            }
            // A speculative Loom renderer revision can retire a tag before
            // its create operation reaches the component-view registry. Such
            // a delete has no native descriptor to recycle. Keep this boundary
            // defensive because descriptor lookup assumes the tag is mounted.
            if (![registry findComponentViewWithTag:operation.tag]) {
              _directEventFamilies.erase(operation.tag);
              _directInstanceHandles.erase(operation.tag);
              _directComponentNames.erase(operation.tag);
              _directSurfaceByTag.erase(operation.tag);
              if (_directEventManager) {
                _directEventManager->unregisterTarget(surfaceId, operation.tag);
              }
              _directPendingStates.erase(operation.tag);
              _directImageProps.erase(operation.tag);
              _directImageStates.erase(operation.tag);
              _directScrollStates.erase(operation.tag);
              _directTextStates.erase(operation.tag);
              _directCompatStates.erase(operation.tag);
              _directLayoutMetrics.erase(operation.tag);
              _directContentInsets.erase(operation.tag);
              _directOverflowInsets.erase(operation.tag);
              return;
            }
            auto descriptor = [registry componentViewDescriptorWithTag:operation.tag];
            [self directDropFinalize:descriptor.view];
            [self directUnregisterTransactionObserver:operation.tag surfaceId:surfaceId];
            const auto handle = [descriptor.view.class componentDescriptorProvider].handle;
            [registry enqueueComponentViewWithComponentHandle:handle
                                                          tag:operation.tag
                                      componentViewDescriptor:descriptor];
            _directEventFamilies.erase(operation.tag);
            _directInstanceHandles.erase(operation.tag);
            _directComponentNames.erase(operation.tag);
            // This branch used to leave `_directSurfaceByTag` behind, so the map
            // grew with every tag the surface ever created and `unregisterSurface`
            // kept iterating dead entries.
            _directSurfaceByTag.erase(operation.tag);
            if (_directEventManager) {
              _directEventManager->unregisterTarget(surfaceId, operation.tag);
            }
            _directPendingStates.erase(operation.tag);
            _directImageProps.erase(operation.tag);
            _directImageStates.erase(operation.tag);
            _directScrollStates.erase(operation.tag);
            _directTextStates.erase(operation.tag);
            _directCompatStates.erase(operation.tag);
            _directLayoutMetrics.erase(operation.tag);
            _directContentInsets.erase(operation.tag);
            _directOverflowInsets.erase(operation.tag);
          }
        },
        batch.operations.at(operationIndex));
  };

  for (const auto &phase : batch.phaseOperationIndexes) {
    for (const auto operationIndex : phase) {
      applyOperation(operationIndex);
    }
  }
  _directReplacedTags.clear();
  [self directFinalizeUpdates];
  LoomTextSetApplyingAnimationFrame(false);
  [self directNotifyTransactionObservers:surfaceId
                                  before:NO
                             transaction:transactionForObservers];
  [_mountingManager.delegate mountingManager:_mountingManager didMountComponentsWithRootTag:surfaceId];
}

- (void)directRegisterTransactionObserver:(UIView<RCTComponentViewProtocol> *)view surfaceId:(SurfaceId)surfaceId
{
  if ([view respondsToSelector:@selector(mountingTransactionWillMount:withSurfaceTelemetry:)] ||
      [view respondsToSelector:@selector(mountingTransactionDidMount:withSurfaceTelemetry:)]) {
    _directTransactionObservers[surfaceId][view.tag] = view;
  }
}

- (void)directUnregisterTransactionObserver:(Tag)tag surfaceId:(SurfaceId)surfaceId
{
  auto observers = _directTransactionObservers.find(surfaceId);
  if (observers != _directTransactionObservers.end()) {
    observers->second.erase(tag);
    if (observers->second.empty()) {
      _directTransactionObservers.erase(observers);
    }
  }
}

- (void)directNotifyTransactionObservers:(SurfaceId)surfaceId
                                  before:(BOOL)before
                             transaction:(const std::function<const MountingTransaction &()> &)transaction
{
  auto observers = _directTransactionObservers.find(surfaceId);
  if (observers == _directTransactionObservers.end()) {
    return;
  }
  // Observers may mount or unmount views (and so change this map) while being notified.
  NSMutableArray<UIView<RCTMountingTransactionObserving> *> *views = [NSMutableArray new];
  for (const auto &[tag, view] : observers->second) {
    UIView<RCTComponentViewProtocol> *strongView = view;
    if (strongView != nil) {
      [views addObject:(UIView<RCTMountingTransactionObserving> *)strongView];
    }
  }
  const SurfaceTelemetry telemetry;
  for (UIView<RCTMountingTransactionObserving> *view in views) {
    if (before) {
      if ([view respondsToSelector:@selector(mountingTransactionWillMount:withSurfaceTelemetry:)]) {
        [view mountingTransactionWillMount:transaction() withSurfaceTelemetry:telemetry];
      }
    } else if ([view respondsToSelector:@selector(mountingTransactionDidMount:withSurfaceTelemetry:)]) {
      [view mountingTransactionDidMount:transaction() withSurfaceTelemetry:telemetry];
    }
  }
}

// The batch as the ShadowViewMutations Fabric would have produced for it, in the order they are
// applied. Shadow views carry the tag and component name; Loom keeps no props or state objects
// for them.
- (ShadowViewMutationList)directMutationsForBatch:(const LoomMountBatch &)batch
                                      descriptors:(const ComponentDescriptorRegistry &)descriptors
{
  ShadowViewMutationList mutations;
  auto shadowView = [](Tag tag, ComponentName componentName) {
    ShadowView view;
    view.tag = tag;
    view.componentName = componentName;
    return view;
  };
  std::unordered_map<Tag, ComponentName> names;
  for (const auto &phase : batch.phaseOperationIndexes) {
    for (const auto operationIndex : phase) {
      std::visit(
          [&](const auto &operation) {
            using Operation = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<Operation, DirectCreateOperation>) {
              if (operation.layoutable) {
                const auto name = descriptors.at(operation.componentName).getComponentName();
                names[operation.tag] = name;
                mutations.push_back(ShadowViewMutation::CreateMutation(shadowView(operation.tag, name)));
              }
            } else if constexpr (std::is_same_v<Operation, DirectUpdatePropsOperation>) {
              auto view = shadowView(operation.tag, names.count(operation.tag) ? names[operation.tag] : nullptr);
              mutations.push_back(ShadowViewMutation::UpdateMutation(view, view, kNoTag));
            } else if constexpr (std::is_same_v<Operation, DirectInsertOperation>) {
              mutations.push_back(ShadowViewMutation::InsertMutation(
                  operation.parentTag, shadowView(operation.tag, nullptr), static_cast<int>(operation.index)));
            } else if constexpr (std::is_same_v<Operation, DirectRemoveOperation>) {
              mutations.push_back(ShadowViewMutation::RemoveMutation(
                  operation.parentTag, shadowView(operation.tag, nullptr), static_cast<int>(operation.index)));
            } else if constexpr (std::is_same_v<Operation, DirectDeleteOperation>) {
              mutations.push_back(ShadowViewMutation::DeleteMutation(shadowView(operation.tag, nullptr)));
            }
          },
          batch.operations.at(operationIndex));
    }
  }
  return mutations;
}

- (void)schedulerDirectRendererFailSurface:(SurfaceId)surfaceId reason:(const std::string &)reason
{
  RCTLogError(@"Direct iOS renderer failed surface %d: %s", surfaceId, reason.c_str());
}

- (void)schedulerDirectRendererRegisterEventTarget:(SurfaceId)surfaceId
                                               tag:(Tag)tag
                                    instanceHandle:(InstanceHandle::Shared)instanceHandle
{
  if (!instanceHandle) {
    return;
  }
  _directSurfaceByTag[tag] = surfaceId;
  _directInstanceHandles[tag] = instanceHandle;
  auto scheduler = [self scheduler];
  if (scheduler) {
    if (!_directEventManager) {
      _directEventManager =
          std::make_shared<LoomEventManager>(scheduler.directRendererEventDispatcher);
    }
    // Register before the component-view lookup: flattened tags have no view,
    // and they are exactly the ones whose onLayout/responder events were being
    // dropped.
    _directEventManager->registerTarget(surfaceId, tag, instanceHandle);
  }
  const auto componentName = _directComponentNames.find(tag);
  if (auto *view = [_mountingManager.componentViewRegistry findComponentViewWithTag:tag];
      view && componentName != _directComponentNames.end() && scheduler) {
    auto descriptors = scheduler.directRendererComponentDescriptorRegistry;
    if (!descriptors) {
      return;
    }
    auto eventEmitter = [self directEventEmitterForSurface:surfaceId
                                                       tag:tag
                                       componentDescriptor:descriptors->at(componentName->second)
                                            instanceHandle:std::move(instanceHandle)];
    [view updateEventEmitter:eventEmitter];
    [self directDeferFinalize:view mask:RNComponentViewUpdateMaskEventEmitter];
  }
}

- (void)schedulerDirectRendererUnregisterEventTarget:(SurfaceId)surfaceId tag:(Tag)tag
{
  if (_directEventManager) {
    _directEventManager->unregisterTarget(surfaceId, tag);
  }
  _directEventFamilies.erase(tag);
  _directInstanceHandles.erase(tag);
  // Component views require a typed, non-null EventEmitter in
  // updateEventEmitter:. The corresponding delete/recycle operation resets
  // the emitter through prepareForRecycle; a replacement target will install
  // its emitter when it is registered. Clearing it here passes nullptr to
  // RCTViewComponentView, whose type assertion intentionally rejects it.
}

- (void)schedulerDirectRendererDispatchEvent:(SurfaceId)surfaceId
                                         tag:(Tag)tag
                                       name:(const std::string &)name
                                     payload:(folly::dynamic)payload
                                    category:(RawEvent::Category)category
                                      unique:(BOOL)unique
{
  if (!_directEventManager) {
    return;
  }
  // The manager owns the target registry, so a tag without a component view
  // (flattened View, virtual text) still dispatches.
  _directEventManager->dispatch(
      surfaceId,
      tag,
      name,
      std::move(payload),
      category,
      unique,
      HighResTimeStamp::now());
}

- (BOOL)schedulerDirectRendererCanMeasureTextOnWorker
{
  RCTAssertMainQueue();
  const auto scale = RCTScreenScale();
  const auto multiplier = RCTFontSizeMultiplier();
#if LOOM_WORKER
  if (_directTextScale.load() != scale || _directFontSizeMultiplier.load() != multiplier) {
    // Do not mutate settings observed by an already published text packet.
    if (RCTScheduler *scheduler = [self scheduler]) {
      LoomBinding::synchronizeWorkerForUIManager(*scheduler.uiManager);
    }
  }
#endif
  _directTextScale.store(scale);
  _directFontSizeMultiplier.store(multiplier);
  return !RCTHasFontHandlerSet();
}

- (float)loomFontSizeMultiplier
{
#if LOOM_WORKER
  return _directFontSizeMultiplier.load();
#else
  return RCTFontSizeMultiplier();
#endif
}

- (std::optional<facebook::react::Size>)schedulerDirectRendererMeasureText:(SurfaceId)surfaceId
                                          attributedString:(const AttributedString &)attributedString
                                       paragraphAttributes:(const ParagraphAttributes &)paragraphAttributes
                                              minimumSize:(const facebook::react::Size &)minimumSize
                                              maximumSize:(const facebook::react::Size &)maximumSize
{
  @autoreleasepool {
  if (!_directTextLayoutManager) {
    _directTextLayoutManager = std::make_shared<TextLayoutManager>(_contextContainer);
  }
  TextLayoutContext layoutContext;
  layoutContext.surfaceId = surfaceId;
#if LOOM_WORKER
  layoutContext.pointScaleFactor = _directTextScale.load();
#else
  layoutContext.pointScaleFactor = RCTScreenScale();
#endif
  LayoutConstraints constraints;
  constraints.minimumSize = minimumSize;
  constraints.maximumSize = maximumSize;
  return _directTextLayoutManager
      ->measure(AttributedStringBox{attributedString}, paragraphAttributes, layoutContext, constraints)
      .size;
  }
}

- (LoomTextMeasurement)schedulerDirectRendererPrepareText:(SurfaceId)surfaceId
                                           attributedString:(const AttributedString &)attributedString
                                        paragraphAttributes:(const ParagraphAttributes &)paragraphAttributes
                                               minimumSize:(const facebook::react::Size &)minimumSize
                                               maximumSize:(const facebook::react::Size &)maximumSize
{
  LoomTextMeasurement result;
  result.size = *[self schedulerDirectRendererMeasureText:surfaceId
                                         attributedString:attributedString
                                      paragraphAttributes:paragraphAttributes
                                             minimumSize:minimumSize
                                             maximumSize:maximumSize];
  result.minimumSize = minimumSize;
  result.maximumSize = maximumSize;
#if LOOM_WORKER
  result.pointScaleFactor = _directTextScale.load();
#else
  result.pointScaleFactor = RCTScreenScale();
#endif
  result.valid = true;
  return result;
}

- (LoomTextMeasurement)schedulerDirectRendererPrepareTextForMount:(SurfaceId)surfaceId
                                                  attributedString:(const AttributedString &)attributedString
                                               paragraphAttributes:(const ParagraphAttributes &)paragraphAttributes
                                                      minimumSize:(const facebook::react::Size &)minimumSize
                                                      maximumSize:(const facebook::react::Size &)maximumSize
{
  return [self schedulerDirectRendererPrepareText:surfaceId
                                  attributedString:attributedString
                               paragraphAttributes:paragraphAttributes
                                      minimumSize:minimumSize
                                      maximumSize:maximumSize];
}

- (void)schedulerDirectRendererUpdateTextState:(const facebook::react::LoomTextStateUpdate &)update
{
  if (update.kind != LoomTextStateKind::Text && update.kind != LoomTextStateKind::Paragraph) {
    return;
  }
  const SurfaceId surfaceId = update.surfaceId;
  const Tag tag = update.tag;
  const auto &layoutSize = update.prepared.size;
  _directSurfaceByTag[tag] = surfaceId;
  if (!_directTextLayoutManager) {
    _directTextLayoutManager = std::make_shared<TextLayoutManager>(_contextContainer);
  }
  ParagraphShadowNode::ConcreteState::Shared state;
  auto &last = _directTextStates[tag];
  if (last.state && last.attributedString == update.attributedString &&
      last.paragraphAttributes == update.paragraphAttributes && last.layoutSize == layoutSize) {
    state = last.state;
  } else {
    // RCTParagraphComponentView draws into state.layoutSize (the content box, as ParagraphShadowNode
    // sets it); a zero size would draw the paragraph unwrapped. The string copy here is the one the
    // platform state type requires.
    ParagraphState data{
        .attributedString = *update.attributedString,
        .paragraphAttributes = update.paragraphAttributes,
        .layoutManager = _directTextLayoutManager,
        .layoutSize = layoutSize};
    auto sharedData = std::make_shared<const ParagraphState>(std::move(data));
    state = std::make_shared<const ParagraphShadowNode::ConcreteState>(sharedData, ShadowNodeFamily::Weak{});
    last.attributedString = update.attributedString;
    last.paragraphAttributes = update.paragraphAttributes;
    last.layoutSize = layoutSize;
    last.state = state;
  }
  if (auto *view = [_mountingManager.componentViewRegistry findComponentViewWithTag:tag]) {
    [view updateState:state oldState:nullptr];
    [self directDeferFinalize:view mask:RNComponentViewUpdateMaskState];
  } else {
    _directPendingStates[tag] = std::move(state);
  }
}

- (void)schedulerDirectRendererUpdateComponentState:(SurfaceId)surfaceId
                                                tag:(Tag)tag
                                              state:(const State::Shared &)state
{
  // Fabric compatibility layer: the state object of the component's own ShadowNode (made by its
  // ComponentDescriptor, with a live family), so the view's `state->updateState(...)` reaches
  // the Loom renderer the way it reaches the shadow tree in Fabric.
  _directSurfaceByTag[tag] = surfaceId;
  auto oldState = _directCompatStates.find(tag);
  if (auto *view = [_mountingManager.componentViewRegistry findComponentViewWithTag:tag]) {
    [view updateState:state oldState:oldState == _directCompatStates.end() ? nullptr : oldState->second];
    [self directDeferFinalize:view mask:RNComponentViewUpdateMaskState];
  } else {
    _directPendingStates[tag] = state;
  }
  _directCompatStates[tag] = state;
}

- (void)schedulerDirectRendererUpdateScrollContentBounds:(SurfaceId)surfaceId
                                                       tag:(Tag)tag
                                                      props:(const folly::dynamic &)props
                                                     bounds:(const facebook::react::Rect &)bounds
{
  _directSurfaceByTag[tag] = surfaceId;
  facebook::react::Point contentOffset{};
  if (props.isObject() && props.count("contentOffset") != 0) {
    const auto &offset = props["contentOffset"];
    if (offset.isObject()) {
      const auto *x = offset.get_ptr("x");
      const auto *y = offset.get_ptr("y");
      if (x != nullptr && x->isNumber()) {
        contentOffset.x = static_cast<Float>(x->asDouble());
      }
      if (y != nullptr && y->isNumber()) {
        contentOffset.y = static_cast<Float>(y->asDouble());
      }
    }
  }
  ScrollViewState data{contentOffset, bounds, 0};
  auto stateData = std::make_shared<const ScrollViewState>(std::move(data));
  auto state = std::make_shared<const ScrollViewShadowNode::ConcreteState>(
      std::move(stateData), ShadowNodeFamily::Weak{});
  auto oldState = _directScrollStates.find(tag);
  if (auto *view = [_mountingManager.componentViewRegistry findComponentViewWithTag:tag]) {
    [view updateState:state
             oldState:oldState == _directScrollStates.end() ? nullptr : oldState->second];
    [self directDeferFinalize:view mask:RNComponentViewUpdateMaskState];
  } else {
    _directPendingStates[tag] = state;
  }
  _directScrollStates[tag] = std::move(state);
}

- (void)schedulerDirectRendererDispatchCommand:(SurfaceId)surfaceId
                                           tag:(Tag)tag
                                         name:(const std::string &)name
                                         args:(const folly::dynamic &)args
{
  (void)surfaceId;
  [_mountingManager dispatchCommand:tag
                        commandName:RCTNSStringFromString(name)
                               args:convertFollyDynamicToId(args)];
}

- (void)schedulerDirectRendererAccessibilityEvent:(SurfaceId)surfaceId tag:(Tag)tag name:(const std::string &)name
{
  (void)surfaceId;
  [_mountingManager sendAccessibilityEvent:tag eventType:RCTNSStringFromString(name)];
}

- (BOOL)schedulerDirectRendererViewLocationInSurface:(SurfaceId)surfaceId tag:(Tag)tag location:(CGPoint *)location
{
  if (!NSThread.isMainThread) {
    return NO;
  }
  RCTComponentViewRegistry *registry = _mountingManager.componentViewRegistry;
  UIView *view = [registry findComponentViewWithTag:tag];
  UIView *root = [registry findComponentViewWithTag:surfaceId];
  if (view == nil || root == nil || view.window == nil || view.window != root.window) {
    return NO;
  }
  // From the center, so the view's own transform (which an animation of its
  // props carries) does not move the origin.
  const CGRect bounds = view.bounds;
  const CGPoint center = [view convertPoint:CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds)) toView:root];
  *location = CGPointMake(center.x - bounds.size.width / 2, center.y - bounds.size.height / 2);
  return YES;
}

- (void)schedulerDirectRendererSetJSResponder:(SurfaceId)surfaceId
                                           tag:(Tag)tag
                                     isResponder:(BOOL)isResponder
                            blockNativeResponder:(BOOL)blockNativeResponder
{
  (void)surfaceId;
  (void)blockNativeResponder;
  [[_mountingManager.componentViewRegistry findComponentViewWithTag:tag] setIsJSResponder:isResponder];
}

#endif

- (void)setupAnimationDriverWithSurfaceHandler:(const facebook::react::SurfaceHandler &)surfaceHandler
{
  [[self scheduler] setupAnimationDriver:surfaceHandler];
}

- (BOOL)suspend
{
  std::lock_guard<std::mutex> lock(_schedulerLifeCycleMutex);

  RCTScheduler *scheduler;
  {
    std::lock_guard<std::mutex> accessLock(_schedulerAccessMutex);

    if (!_scheduler) {
      return NO;
    }
    scheduler = _scheduler;
    _scheduler = nil;
  }

#if LOOM_WORKER
  LoomBinding::shutdownWorkerForUIManager(*scheduler.uiManager);
#endif
  [self _stopAllSurfacesWithScheduler:scheduler];

  return YES;
}

- (BOOL)resume
{
  std::lock_guard<std::mutex> lock(_schedulerLifeCycleMutex);

  RCTScheduler *scheduler;
  {
    std::lock_guard<std::mutex> accessLock(_schedulerAccessMutex);

    if (_scheduler) {
      return NO;
    }
    scheduler = [self _createScheduler];
  }

  [self _startAllSurfacesWithScheduler:scheduler];

  {
    std::lock_guard<std::mutex> accessLock(_schedulerAccessMutex);
    _scheduler = scheduler;
  }

  return YES;
}

#pragma mark - Private

- (RCTScheduler *)_createScheduler
{
  auto componentRegistryFactory = [factory =
                                       wrapManagedObject(_mountingManager.componentViewRegistry.componentViewFactory)](
                                      const EventDispatcher::Weak &eventDispatcher,
                                      const std::shared_ptr<const ContextContainer> &contextContainer) {
    return [(RCTComponentViewFactory *)unwrapManagedObject(factory)
        createComponentDescriptorRegistryWithParameters:{.eventDispatcher = eventDispatcher,
                                                         .contextContainer = contextContainer}];
  };

  auto runtimeExecutor = _runtimeExecutor;

  auto toolbox = SchedulerToolbox{};
  toolbox.contextContainer = _contextContainer;
  toolbox.componentRegistryFactory = componentRegistryFactory;

  auto weakRuntimeScheduler = _contextContainer->find<std::weak_ptr<RuntimeScheduler>>(RuntimeSchedulerKey);
  auto runtimeScheduler = weakRuntimeScheduler.has_value() ? weakRuntimeScheduler.value().lock() : nullptr;
  if (runtimeScheduler) {
    runtimeExecutor = [runtimeScheduler](std::function<void(jsi::Runtime & runtime)> &&callback) {
      runtimeScheduler->scheduleWork(std::move(callback));
    };
  }

  toolbox.runtimeExecutor = runtimeExecutor;
  toolbox.bridgelessBindingsExecutor = _bridgelessBindingsExecutor;

  __weak RCTMountingManager *weakMountingManager = _mountingManager;
  toolbox.eventBeatFactory = [runtimeScheduler, weakMountingManager](
                                 std::shared_ptr<EventBeat::OwnerBox> ownerBox) -> std::unique_ptr<EventBeat> {
    auto runLoopObserver =
        std::make_unique<const MainRunLoopObserver>(RunLoopObserver::Activity::BeforeWaiting, ownerBox->owner);
    auto windowLayerResolver = [weakMountingManager](Tag tag) -> CALayer * {
      RCTMountingManager *mountingManager = weakMountingManager;
      return [mountingManager.componentViewRegistry findComponentViewWithTag:tag].window.layer;
    };
    return std::make_unique<AppleEventBeat>(
        std::move(ownerBox), std::move(runLoopObserver), *runtimeScheduler, std::move(windowLayerResolver));
  };

  RCTScheduler *scheduler = [[RCTScheduler alloc] initWithToolbox:toolbox];
  scheduler.delegate = self;

  return scheduler;
}

- (void)_startAllSurfacesWithScheduler:(RCTScheduler *)scheduler
{
  [_surfaceRegistry enumerateWithBlock:^(NSEnumerator<RCTFabricSurface *> *enumerator) {
    for (RCTFabricSurface *surface in enumerator) {
      [scheduler registerSurface:surface.surfaceHandler];
      [surface start];
    }
  }];
}

- (void)_stopAllSurfacesWithScheduler:(RCTScheduler *)scheduler
{
  [_surfaceRegistry enumerateWithBlock:^(NSEnumerator<RCTFabricSurface *> *enumerator) {
    for (RCTFabricSurface *surface in enumerator) {
      [surface stop];
      [scheduler unregisterSurface:surface.surfaceHandler];
    }
  }];
}

- (void)_applicationWillTerminate
{
  [self suspend];
}

#pragma mark - RCTSchedulerDelegate

- (void)schedulerDidFinishTransaction:(std::shared_ptr<const MountingCoordinator>)mountingCoordinator
{
  // no-op, we will flush the transaction from schedulerShouldRenderTransactions
}

- (void)schedulerShouldRenderTransactions:(std::shared_ptr<const MountingCoordinator>)mountingCoordinator
{
  [_mountingManager scheduleTransaction:mountingCoordinator];
}

- (void)schedulerShouldMergeReactRevision:(SurfaceId)surfaceId
{
  if (RCTIsMainQueue() || !ReactNativeFeatureFlags::enableFabricCommitBranchingMergeOnMainThread()) {
    [self _mergeReactRevisionForSurfaceId:surfaceId];
    return;
  }

  bool needsWake = false;
  {
    std::lock_guard<std::mutex> lock(_pendingReactRevisionMergesMutex);
    needsWake = _pendingReactRevisionMerges.empty();
    _pendingReactRevisionMerges.insert(surfaceId);
  }

  if (needsWake) {
    CFRunLoopWakeUp(CFRunLoopGetMain());
  }
}

- (void)_mergeReactRevisionForSurfaceId:(SurfaceId)surfaceId
{
  RCTScheduler *scheduler = [self scheduler];
  if (!scheduler) {
    return;
  }

  auto uiManager = scheduler.uiManager;
  if (!uiManager) {
    return;
  }

  uiManager->getShadowTreeRegistry().visit(
      surfaceId, [](const ShadowTree &shadowTree) { shadowTree.mergeReactRevision(); });
}

- (void)_drainPendingReactRevisionMerges
{
  RCTAssertMainQueue();

  std::unordered_set<SurfaceId> pending;
  {
    std::lock_guard<std::mutex> lock(_pendingReactRevisionMergesMutex);
    if (_pendingReactRevisionMerges.empty()) {
      return;
    }

    pending.swap(_pendingReactRevisionMerges);
  }

  for (auto surfaceId : pending) {
    [self _mergeReactRevisionForSurfaceId:surfaceId];
  }
}

- (void)schedulerDidDispatchCommand:(const ShadowView &)shadowView
                        commandName:(const std::string &)commandName
                               args:(const folly::dynamic &)args
{
  ReactTag tag = shadowView.tag;
  NSString *commandStr = [[NSString alloc] initWithUTF8String:commandName.c_str()];
  NSArray *argsArray = convertFollyDynamicToId(args);

  [_mountingManager dispatchCommand:tag commandName:commandStr args:argsArray];
}

- (void)schedulerDidSendAccessibilityEvent:(const facebook::react::ShadowView &)shadowView
                                 eventType:(const std::string &)eventType
{
  ReactTag tag = shadowView.tag;
  NSString *eventTypeStr = [[NSString alloc] initWithUTF8String:eventType.c_str()];

  [_mountingManager sendAccessibilityEvent:tag eventType:eventTypeStr];
}

- (void)schedulerDidSetIsJSResponder:(BOOL)isJSResponder
                blockNativeResponder:(BOOL)blockNativeResponder
                       forShadowView:(const facebook::react::ShadowView &)shadowView;
{
  [_mountingManager setIsJSResponder:isJSResponder blockNativeResponder:blockNativeResponder forShadowView:shadowView];
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-implementations"
- (void)addObserver:(id<RCTSurfacePresenterObserver>)observer
{
  std::unique_lock lock(_observerListMutex);
  _observers.push_back(observer);
}
#pragma clang diagnostic pop

- (void)removeObserver:(id<RCTSurfacePresenterObserver>)observer
{
  std::unique_lock lock(_observerListMutex);
  std::vector<__weak id<RCTSurfacePresenterObserver>>::const_iterator it =
      std::find(_observers.begin(), _observers.end(), observer);
  if (it != _observers.end()) {
    _observers.erase(it);
  }
}

#pragma mark - RCTMountingManagerDelegate

- (void)mountingManager:(RCTMountingManager *)mountingManager willMountComponentsWithRootTag:(ReactTag)rootTag
{
  RCTAssertMainQueue();

  NSArray<id<RCTSurfacePresenterObserver>> *observersCopy;
  {
    std::shared_lock lock(_observerListMutex);
    observersCopy = [self _getObservers];
  }

  for (id<RCTSurfacePresenterObserver> observer in observersCopy) {
    if ([observer respondsToSelector:@selector(willMountComponentsWithRootTag:)]) {
      [observer willMountComponentsWithRootTag:rootTag];
    }
  }
}

- (void)mountingManager:(RCTMountingManager *)mountingManager didMountComponentsWithRootTag:(ReactTag)rootTag
{
  RCTAssertMainQueue();

  NSArray<id<RCTSurfacePresenterObserver>> *observersCopy;
  {
    std::shared_lock lock(_observerListMutex);
    observersCopy = [self _getObservers];
  }

  for (id<RCTSurfacePresenterObserver> observer in observersCopy) {
    if ([observer respondsToSelector:@selector(didMountComponentsWithRootTag:)]) {
      [observer didMountComponentsWithRootTag:rootTag];
    }
  }

  RCTScheduler *scheduler = [self scheduler];
  if (scheduler) {
    // Notify mount when the effects are visible and prevent mount hooks to
    // delay paint.
    dispatch_async(dispatch_get_main_queue(), ^{
      [scheduler reportMount:rootTag];
    });
  }
}

- (NSArray<id<RCTSurfacePresenterObserver>> *)_getObservers
{
  NSMutableArray<id<RCTSurfacePresenterObserver>> *observersCopy = [NSMutableArray new];
  for (id<RCTSurfacePresenterObserver> observer : _observers) {
    if (observer) {
      [observersCopy addObject:observer];
    }
  }

  return observersCopy;
}

@end
