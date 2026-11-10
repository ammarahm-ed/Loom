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
#import <React/RCTScheduler.h>
#import <React/RCTSurfaceRegistry.h>
#import <React/RCTSurfaceView+Internal.h>
#import <React/RCTSurfaceView.h>
#import <React/RCTUtils.h>
#import <react/utils/FollyConvert.h>

#import <react/featureflags/ReactNativeFeatureFlags.h>
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
#endif
#import "AppleEventBeat.h"

#import "PlatformRunLoopObserver.h"
#import "RCTConversions.h"

using namespace facebook;
using namespace facebook::react;

@interface RCTSurfacePresenter () <RCTSchedulerDelegate, RCTMountingManagerDelegate>
- (void)_drainPendingReactRevisionMerges;
@end

namespace {
template <typename PropsT>
struct DirectPropsParser final {
  RawPropsParser parser;
  DirectPropsParser() { parser.prepare<PropsT>(); }
};

const RawPropsParser &directParagraphPropsParser()
{
  static const DirectPropsParser<ParagraphProps> parser;
  return parser.parser;
}

const RawPropsParser &directTextPropsParser()
{
  static const DirectPropsParser<TextProps> parser;
  return parser.parser;
}

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
  std::unordered_map<Tag, folly::dynamic> _directImageProps;
  std::unordered_map<Tag, ImageShadowNode::ConcreteState::Shared> _directImageStates;
  std::unordered_map<Tag, ScrollViewShadowNode::ConcreteState::Shared> _directScrollStates;
  std::unordered_map<Tag, LayoutMetrics> _directLayoutMetrics;
  std::unordered_map<Tag, std::array<float, 4>> _directContentInsets;
  std::unordered_map<Tag, std::array<float, 4>> _directOverflowInsets;
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
      self->_directComponentNames.erase(tag);
      self->_directLayoutMetrics.erase(tag);
      self->_directContentInsets.erase(tag);
      self->_directOverflowInsets.erase(tag);
      self->_directSurfaceByTag.erase(tag);
    }
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
- (void)schedulerDirectRendererApplyBatch:(SurfaceId)surfaceId batch:(LoomMountBatch)batch
{
  RCTAssertMainQueue();
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
    [view finalizeUpdates:RNComponentViewUpdateMaskState];
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
            const auto unifiedName = componentNameByReactViewName(operation.componentName);
            _directComponentNames[operation.tag] = unifiedName;
            if (unifiedName == "Image") {
              _directImageProps[operation.tag] = operation.props;
              _directImageStates.erase(operation.tag);
            }
            Props::Shared props = componentDescriptor.cloneProps(context, nullptr, RawProps(operation.props));
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
            [viewDescriptor.view finalizeUpdates:RNComponentViewUpdateMaskProps |
                                                    (eventEmitter ? RNComponentViewUpdateMaskEventEmitter : RNComponentViewUpdateMaskNone)];
            auto pendingState = _directPendingStates.find(operation.tag);
            if (pendingState != _directPendingStates.end()) {
              [viewDescriptor.view updateState:pendingState->second oldState:nullptr];
              [viewDescriptor.view finalizeUpdates:RNComponentViewUpdateMaskState];
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
            auto newProps = componentDescriptor->cloneProps(context, oldProps, RawProps(operation.props));
            [view updateProps:newProps oldProps:oldProps];
            [view finalizeUpdates:RNComponentViewUpdateMaskProps];
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
            [view finalizeUpdates:RNComponentViewUpdateMaskLayoutMetrics];
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
              _directLayoutMetrics.erase(operation.tag);
              _directContentInsets.erase(operation.tag);
              _directOverflowInsets.erase(operation.tag);
              return;
            }
            auto descriptor = [registry componentViewDescriptorWithTag:operation.tag];
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
  [_mountingManager.delegate mountingManager:_mountingManager didMountComponentsWithRootTag:surfaceId];
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
    [view finalizeUpdates:RNComponentViewUpdateMaskEventEmitter];
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

- (TextAttributes)schedulerDirectRendererParseTextAttributes:(SurfaceId)surfaceId
                                                       name:(const std::string &)name
                                                      props:(const folly::dynamic &)props
                                                  inherited:(const TextAttributes &)inherited
                                         paragraphAttributes:(ParagraphAttributes &)paragraphAttributes
{
  auto attributes = inherited;
#if LOOM_WORKER
  attributes.fontSizeMultiplier = _directFontSizeMultiplier.load();
#else
  attributes.fontSizeMultiplier = RCTFontSizeMultiplier();
#endif
  const auto unifiedName = componentNameByReactViewName(name);
  PropsParserContext context{surfaceId, *_contextContainer};
  RawProps rawProps{props};
  if (unifiedName == "Paragraph") {
    rawProps.parse(directParagraphPropsParser());
    ParagraphProps parsed{context, ParagraphProps{}, rawProps};
    attributes.apply(parsed.textAttributes);
    paragraphAttributes = parsed.paragraphAttributes;
  } else if (unifiedName == "Text") {
    rawProps.parse(directTextPropsParser());
    TextProps parsed{context, TextProps{}, rawProps};
    attributes.apply(parsed.textAttributes);
  }
  return attributes;
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

- (void)schedulerDirectRendererUpdateTextState:(SurfaceId)surfaceId
                                            tag:(Tag)tag
                                  componentName:(const std::string &)componentName
                                          props:(const folly::dynamic &)props
                                attributedString:(const AttributedString &)attributedString
                             paragraphAttributes:(const ParagraphAttributes &)paragraphAttributes
{
  (void)props;
  if (componentNameByReactViewName(componentName) != "Paragraph") {
    return;
  }
  _directSurfaceByTag[tag] = surfaceId;
  if (!_directTextLayoutManager) {
    _directTextLayoutManager = std::make_shared<TextLayoutManager>(_contextContainer);
  }
  ParagraphState data{attributedString, paragraphAttributes, _directTextLayoutManager};
  auto sharedData = std::make_shared<const ParagraphState>(std::move(data));
  auto state = std::make_shared<const ParagraphShadowNode::ConcreteState>(
      sharedData, ShadowNodeFamily::Weak{});
  if (auto *view = [_mountingManager.componentViewRegistry findComponentViewWithTag:tag]) {
    [view updateState:state oldState:nullptr];
    [view finalizeUpdates:RNComponentViewUpdateMaskState];
  } else {
    _directPendingStates[tag] = std::move(state);
  }
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
    [view finalizeUpdates:RNComponentViewUpdateMaskState];
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
