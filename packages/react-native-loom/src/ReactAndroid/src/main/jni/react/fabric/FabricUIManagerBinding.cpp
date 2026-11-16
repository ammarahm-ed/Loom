/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FabricUIManagerBinding.h"
#if LOOM_RENDERER
#include <react/renderer/loom/binding/LoomBinding.h>
#endif

#include "AndroidAnimationChoreographer.h"
#include "AndroidEventBeat.h"
#include "ComponentFactory.h"
#include "EventBeatManager.h"
#include "FabricMountingManager.h"
#include "FocusOrderingHelper.h"

#include <cxxreact/TraceSection.h>
#include <fbjni/fbjni.h>
#include <glog/logging.h>
#include <jsi/JSIDynamic.h>
#include <jsi/jsi.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>
#if !LOOM_RENDERER
#include <react/renderer/animations/LayoutAnimationDriver.h>
#endif
#include <react/renderer/componentregistry/ComponentDescriptorFactory.h>
#include <react/renderer/core/EventBeat.h>
#include <react/renderer/core/EventEmitter.h>
#include <react/renderer/attributedstring/AttributedStringBox.h>
#include <react/renderer/componentregistry/componentNameByReactViewName.h>
#include <react/renderer/core/LayoutConstraints.h>
#include <react/renderer/textlayoutmanager/TextLayoutManager.h>
#include <react/renderer/components/text/ParagraphProps.h>
#include <react/renderer/components/text/TextProps.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawPropsParser.h>
#include <react/renderer/core/conversions.h>
#include <react/renderer/imagemanager/ImageFetcher.h>
#include <react/renderer/scheduler/Scheduler.h>
#include <react/renderer/scheduler/SchedulerDelegate.h>
#include <react/renderer/scheduler/SchedulerToolbox.h>
#include <react/renderer/uimanager/primitives.h>
#include <react/utils/ContextContainer.h>
#include <string_view>
#include <fbjni/NativeRunnable.h>
#include <algorithm>

#if LOOM_RENDERER
#include <react/renderer/loom/core/LoomRenderer.h>
#include <react/renderer/loom/host/LoomHost.h>
#include <mutex>
#include <thread>
#include "LoomAndroidHost.h"

#endif

namespace facebook::react {

void FabricUIManagerBinding::initHybrid(jni::alias_ref<jhybridobject> jobj) {
  setCxxInstance(jobj);
}

// Thread-safe getter
std::shared_ptr<Scheduler> FabricUIManagerBinding::getScheduler() {
  std::shared_lock lock(installMutex_);
  // Need to return a copy of the shared_ptr to make sure this is safe if called
  // concurrently with uninstallFabricUIManager
  return scheduler_;
}

void FabricUIManagerBinding::setPixelDensity(float pointScaleFactor) {
#if LOOM_RENDERER
  if (pointScaleFactor != pointScaleFactor_.load()) {
    auto scheduler = getScheduler();
    if (scheduler) LoomBinding::synchronizeWorkerForUIManager(*scheduler->getUIManager());
  }
#endif
  pointScaleFactor_ = pointScaleFactor;
#if LOOM_RENDERER
  LoomBinding::setPointScaleFactor(pointScaleFactor);
#endif
}

void FabricUIManagerBinding::driveCxxAnimations() {
  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR)
        << "FabricUIManagerBinding::driveCxxAnimations: scheduler disappeared";
    return;
  }
  scheduler->animationTick();
}

namespace {
/**
 * Loom: tells FabricUIManager while the shared animation backend has a
 * registered animation, so its frame callback calls into the backend only
 * then instead of on every frame.
 */
class LoomAnimationChoreographer final : public AndroidAnimationChoreographer {
 public:
  explicit LoomAnimationChoreographer(
      jni::global_ref<JFabricUIManager::javaobject> uiManager)
      : uiManager_(std::move(uiManager)) {}

  void resume() override {
    AndroidAnimationChoreographer::resume();
    notify(true);
  }

  void pause() override {
    AndroidAnimationChoreographer::pause();
    notify(false);
  }

 private:
  void notify(bool active) {
    jni::ThreadScope::WithClassLoader([&] {
      static const auto setActive =
          JFabricUIManager::javaClassStatic()->getMethod<void(jboolean)>(
              "setAnimationBackendActive");
      setActive(uiManager_, static_cast<jboolean>(active));
    });
  }

  jni::global_ref<JFabricUIManager::javaobject> uiManager_;
};
} // namespace

void FabricUIManagerBinding::driveAnimationBackend(jlong frameTimeNanos) {
  if (!animationChoreographer_) {
    LOG(ERROR)
        << "FabricUIManagerBinding::driveAnimationBackend: animation choreographer disappeared";
    return;
  }
  auto frameTimeMs = static_cast<double>(frameTimeNanos) / 1000000.0;
  animationChoreographer_->onAnimationFrameIfActive(
      AnimationTimestamp{frameTimeMs});
}

void FabricUIManagerBinding::drainPreallocateViewsQueue() {
  auto mountingManager = getMountingManager("drainPreallocateViewsQueue");
  if (!mountingManager) {
    return;
  }
  mountingManager->drainPreallocateViewsQueue();
}

void FabricUIManagerBinding::reportMount(SurfaceId surfaceId) {
  {
    // This is a fix for `MountingCoordinator::hasPendingTransactions` on
    // Android, which otherwise would report no pending transactions
    // incorrectly. This is due to the push model used on Android and can be
    // removed when we migrate to a pull model.
    std::shared_lock lock(surfaceHandlerRegistryMutex_);
    auto iterator = surfaceHandlerRegistry_.find(surfaceId);
    if (iterator != surfaceHandlerRegistry_.end()) {
      const auto* surfaceHandler =
          std::get_if<SurfaceHandler>(&iterator->second);
      if (surfaceHandler == nullptr) {
        auto javaSurfaceHandler =
            std::get<jni::weak_ref<SurfaceHandlerBinding::jhybridobject>>(
                iterator->second)
                .lockLocal();
        if (javaSurfaceHandler) {
          surfaceHandler = &javaSurfaceHandler->cthis()->getSurfaceHandler();
        }
      }
      if (surfaceHandler != nullptr) {
        auto mountingCoordinator = surfaceHandler->getMountingCoordinator();
#if !LOOM_RENDERER
        if (mountingCoordinator != nullptr) {
          mountingCoordinator->didPerformAsyncTransactions();
        }
#else
        (void)mountingCoordinator;
#endif
      }
    } else {
      LOG(ERROR) << "FabricUIManagerBinding::reportMount: Surface with id "
                 << surfaceId << " is not found";
    }
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "FabricUIManagerBinding::reportMount: scheduler disappeared";
    return;
  }
  scheduler->reportMount(surfaceId);
}

void FabricUIManagerBinding::pullAndExecuteTransaction(SurfaceId surfaceId) {
  TraceSection section("FabricUIManagerBinding::pullAndExecuteTransaction");

  std::shared_ptr<const MountingCoordinator> mountingCoordinator;
  {
    std::scoped_lock lock(pullableMountingCoordinatorsMutex_);
    auto pullable = pullableMountingCoordinators_.extract(surfaceId);
    if (pullable.empty()) {
      return;
    }
    mountingCoordinator = std::move(pullable.mapped());
  }

  auto mountingManager = getMountingManager("pullAndExecuteTransaction");
  if (!mountingManager) {
    return;
  }

  // The UI thread pulls the transaction itself (it may accumulate several
  // revisions committed since the notification was enqueued, and may be empty
  // if a previous pull already consumed them). willPerformAsynchronously =
  // false: the transaction is applied synchronously right here, so no
  // `didPerformAsyncTransactions` bookkeeping is needed.
  auto mountingTransaction =
      mountingCoordinator->pullTransaction(/* willPerformAsynchronously = */
                                           false);
  if (mountingTransaction.has_value()) {
    mountingManager->executeMount(
        *mountingTransaction, /* synchronous = */ true);
  }
}

#pragma mark - Surface management

// Used by bridgeless
void FabricUIManagerBinding::startSurfaceWithSurfaceHandler(
    jint surfaceId,
    jni::alias_ref<SurfaceHandlerBinding::jhybridobject> surfaceHandlerBinding,
    jboolean isMountable) {
  TraceSection s("FabricUIManagerBinding::startSurfaceWithSurfaceHandler");
  if (enableFabricLogs_) {
    LOG(WARNING)
        << "FabricUIManagerBinding::startSurfaceWithSurfaceHandler() was called (address: "
        << this << ", surfaceId: " << surfaceId << ").";
  }

  const auto& surfaceHandler =
      surfaceHandlerBinding->cthis()->getSurfaceHandler();
  surfaceHandler.setSurfaceId(surfaceId);
  surfaceHandler.setDisplayMode(
      isMountable != 0 ? DisplayMode::Visible : DisplayMode::Suspended);

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR)
        << "FabricUIManagerBinding::startSurfaceWithSurfaceHandler: scheduler disappeared";
    return;
  }
  scheduler->registerSurface(surfaceHandler);

  auto mountingManager = getMountingManager("startSurfaceWithSurfaceHandler");
  if (mountingManager != nullptr) {
    mountingManager->onSurfaceStart(surfaceId);
  }

  surfaceHandler.start();

#if !LOOM_RENDERER // Loom animates layout itself (LoomLayoutAnimations)
  if (ReactNativeFeatureFlags::enableLayoutAnimationsOnAndroid()) {
    surfaceHandler.getMountingCoordinator()->setMountingOverrideDelegate(
        animationDriver_);
  }
#endif

  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    surfaceHandlerRegistry_.emplace(
        surfaceId, jni::make_weak(surfaceHandlerBinding));
  }
}

// Used by non-bridgeless+Fabric
void FabricUIManagerBinding::startSurface(
    jint surfaceId,
    jni::alias_ref<jstring> moduleName,
    NativeMap* initialProps) {
  TraceSection s("FabricUIManagerBinding::startSurface");

  if (enableFabricLogs_) {
    LOG(WARNING)
        << "FabricUIManagerBinding::startSurface() was called (address: "
        << this << ", surfaceId: " << surfaceId << ").";
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "FabricUIManagerBinding::startSurface: scheduler disappeared";
    return;
  }

  SurfaceHandler* surfaceHandler = nullptr;
  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    auto [it, _] = surfaceHandlerRegistry_.try_emplace(
        surfaceId,
        std::in_place_index<0>,
        moduleName->toStdString(),
        surfaceId);
    surfaceHandler = &std::get<SurfaceHandler>(it->second);
  }

  surfaceHandler->setContextContainer(scheduler->getContextContainer());
  if (initialProps != nullptr) {
    surfaceHandler->setProps(initialProps->consume());
  }

  auto layoutContext = LayoutContext{};
  layoutContext.pointScaleFactor = pointScaleFactor_;
  surfaceHandler->constraintLayout({}, layoutContext);

  scheduler->registerSurface(*surfaceHandler);

  auto mountingManager = getMountingManager("startSurface");
  if (mountingManager != nullptr) {
    mountingManager->onSurfaceStart(surfaceId);
  }

  surfaceHandler->start();

#if !LOOM_RENDERER
  if (ReactNativeFeatureFlags::enableLayoutAnimationsOnAndroid()) {
    surfaceHandler->getMountingCoordinator()->setMountingOverrideDelegate(
        animationDriver_);
  }
#endif
}

jint FabricUIManagerBinding::findNextFocusableElement(
    jint parentTag,
    jint focusedTag,
    jint direction) {
  std::shared_ptr<const ShadowNode> nextNode;

  std::optional<FocusDirection> focusDirection =
      FocusOrderingHelper::resolveFocusDirection(direction);

  if (!focusDirection.has_value()) {
    return -1;
  }

  std::shared_ptr<UIManager> uimanager = getScheduler()->getUIManager();

  std::shared_ptr<const ShadowNode> parentShadowNode =
      uimanager->findShadowNodeByTag_DEPRECATED(parentTag);

  if (parentShadowNode == nullptr) {
    return -1;
  }

  std::shared_ptr<const ShadowNode> focusedShadowNode =
      FocusOrderingHelper::findShadowNodeByTagRecursively(
          parentShadowNode, focusedTag);

  if (focusedShadowNode == nullptr) {
    return -1;
  }

  LayoutMetrics childLayoutMetrics = uimanager->getRelativeLayoutMetrics(
      *focusedShadowNode, parentShadowNode.get(), {.includeTransform = true});

  Rect sourceRect = childLayoutMetrics.frame;

  /*
   * Traverse the tree recursively to find the next focusable element in the
   * given direction
   */
  std::optional<Rect> nextRect = std::nullopt;
  FocusOrderingHelper::traverseAndUpdateNextFocusableElement(
      parentShadowNode,
      focusedShadowNode,
      parentShadowNode,
      focusDirection.value(),
      *uimanager,
      sourceRect,
      nextRect,
      nextNode);

  if (nextNode == nullptr) {
    return -1;
  }

  return nextNode->getTag();
}

jintArray FabricUIManagerBinding::getRelativeAncestorList(
    jint rootTag,
    jint childTag) {
  JNIEnv* env = jni::Environment::current();

  std::shared_ptr<UIManager> uimanager = getScheduler()->getUIManager();

  std::shared_ptr<const ShadowNode> childShadowNode =
      uimanager->findShadowNodeByTag_DEPRECATED(childTag);
  std::shared_ptr<const ShadowNode> rootShadowNode =
      uimanager->findShadowNodeByTag_DEPRECATED(rootTag);

  if (childShadowNode == nullptr || rootShadowNode == nullptr) {
    return nullptr;
  }

  ShadowNode::AncestorList ancestorList =
      childShadowNode->getFamily().getAncestors(*rootShadowNode);

  if (ancestorList.empty() || ancestorList.size() < 2) {
    return nullptr;
  }

  // ignore the first ancestor as it is the rootShadowNode itself
  std::vector<int> ancestorTags;
  for (auto it = std::next(ancestorList.begin()); it != ancestorList.end();
       ++it) {
    auto& ancestor = *it;
    ancestorTags.push_back(ancestor.first.get().getTag());
  }

  jintArray result = env->NewIntArray(static_cast<jint>(ancestorTags.size()));

  env->SetIntArrayRegion(
      result, 0, static_cast<jint>(ancestorTags.size()), ancestorTags.data());

  return result;
}

// Used by non-bridgeless+Fabric
void FabricUIManagerBinding::startSurfaceWithConstraints(
    jint surfaceId,
    jni::alias_ref<jstring> moduleName,
    NativeMap* initialProps,
    jfloat minWidth,
    jfloat maxWidth,
    jfloat minHeight,
    jfloat maxHeight,
    jfloat offsetX,
    jfloat offsetY,
    jboolean isRTL,
    jboolean doLeftAndRightSwapInRTL) {
  TraceSection s("FabricUIManagerBinding::startSurfaceWithConstraints");

  if (enableFabricLogs_) {
    LOG(WARNING)
        << "FabricUIManagerBinding::startSurfaceWithConstraints() was called (address: "
        << this << ", surfaceId: " << surfaceId << ").";
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR)
        << "FabricUIManagerBinding::startSurfaceWithConstraints: scheduler disappeared";
    return;
  }

  auto minimumSize = Size{
      .width = minWidth / pointScaleFactor_,
      .height = minHeight / pointScaleFactor_};
  auto maximumSize = Size{
      .width = maxWidth / pointScaleFactor_,
      .height = maxHeight / pointScaleFactor_};

#if LOOM_RENDERER
  // The Loom renderer does not consume SurfaceHandler's legacy Fabric
  // revision/layout publication. Give it the initial viewport explicitly,
  // already converted to DIP so that everything downstream is in one space.
  LoomBinding::setSurfaceConstraints(
      surfaceId,
      minimumSize.width,
      minimumSize.height,
      maximumSize.width,
      maximumSize.height,
      isRTL != 0,
      offsetX / pointScaleFactor_,
      offsetY / pointScaleFactor_);
#endif

  LayoutContext context;
  context.viewportOffset =
      Point{.x = offsetX / pointScaleFactor_, .y = offsetY / pointScaleFactor_};
  context.pointScaleFactor = {pointScaleFactor_};
  context.swapLeftAndRightInRTL = doLeftAndRightSwapInRTL != 0;
  LayoutConstraints constraints = {};
  constraints.minimumSize = minimumSize;
  constraints.maximumSize = maximumSize;
  constraints.layoutDirection =
      isRTL != 0 ? LayoutDirection::RightToLeft : LayoutDirection::LeftToRight;

  SurfaceHandler* surfaceHandler = nullptr;
  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    auto [it, _] = surfaceHandlerRegistry_.try_emplace(
        surfaceId,
        std::in_place_index<0>,
        moduleName->toStdString(),
        surfaceId);
    surfaceHandler = &std::get<SurfaceHandler>(it->second);
  }

  surfaceHandler->setContextContainer(scheduler->getContextContainer());
  if (initialProps != nullptr) {
    surfaceHandler->setProps(initialProps->consume());
  }
  surfaceHandler->constraintLayout(constraints, context);

  scheduler->registerSurface(*surfaceHandler);

  auto mountingManager = getMountingManager("startSurfaceWithConstraints");
  if (mountingManager != nullptr) {
    mountingManager->onSurfaceStart(surfaceId);
  }

  surfaceHandler->start();

#if !LOOM_RENDERER
  if (ReactNativeFeatureFlags::enableLayoutAnimationsOnAndroid()) {
    surfaceHandler->getMountingCoordinator()->setMountingOverrideDelegate(
        animationDriver_);
  }
#endif
}

// Used by non-bridgeless+Fabric
void FabricUIManagerBinding::stopSurface(jint surfaceId) {
  TraceSection s("FabricUIManagerBinding::stopSurface");
  if (enableFabricLogs_) {
    LOG(WARNING)
        << "FabricUIManagerBinding::stopSurface() was called (address: " << this
        << ", surfaceId: " << surfaceId << ").";
  }

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR) << "FabricUIManagerBinding::stopSurface: scheduler disappeared";
    return;
  }

  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    auto iterator = surfaceHandlerRegistry_.find(surfaceId);
    if (iterator == surfaceHandlerRegistry_.end()) {
      LOG(ERROR)
          << "FabricUIManagerBinding::stopSurface: Surface with given id is not found";
      return;
    }

    auto* surfaceHandler = std::get_if<SurfaceHandler>(&iterator->second);
    if (surfaceHandler != nullptr) {
      surfaceHandler->stop();
      scheduler->unregisterSurface(*surfaceHandler);
    } else {
      LOG(ERROR) << "Java-owned SurfaceHandler found in stopSurface";
    }
    surfaceHandlerRegistry_.erase(iterator);
  }

  auto mountingManager = getMountingManager("stopSurface");
  if (!mountingManager) {
    return;
  }
  mountingManager->onSurfaceStop(surfaceId);
#if LOOM_RENDERER
  // The Loom renderer keeps its own Yoga tree and tag bookkeeping per
  // surface; a restarted surface must not inherit it.
  LoomBinding::releaseSurface(surfaceId);
#endif
}

// Used by bridgeless
void FabricUIManagerBinding::stopSurfaceWithSurfaceHandler(
    jni::alias_ref<SurfaceHandlerBinding::jhybridobject>
        surfaceHandlerBinding) {
  TraceSection s("FabricUIManagerBinding::stopSurfaceWithSurfaceHandler");
  const auto& surfaceHandler =
      surfaceHandlerBinding->cthis()->getSurfaceHandler();
  if (enableFabricLogs_) {
    LOG(WARNING)
        << "FabricUIManagerBinding::stopSurfaceWithSurfaceHandler() was called (address: "
        << this << ", surfaceId: " << surfaceHandler.getSurfaceId() << ").";
  }

  // This is necessary to make sure we remove the surface handler from the
  // registry before invalidating it. Otherwise, we can access an invalid
  // reference in `reportMount`.
  {
    std::unique_lock lock(surfaceHandlerRegistryMutex_);
    surfaceHandlerRegistry_.erase(surfaceHandler.getSurfaceId());
  }

  surfaceHandler.stop();

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR)
        << "FabricUIManagerBinding::unregisterSurface: scheduler disappeared";
    return;
  }
  scheduler->unregisterSurface(surfaceHandler);

  auto mountingManager = getMountingManager("unregisterSurface");
  if (!mountingManager) {
    return;
  }
  mountingManager->onSurfaceStop(surfaceHandler.getSurfaceId());
#if LOOM_RENDERER
  LoomBinding::releaseSurface(surfaceHandler.getSurfaceId());
#endif
}

void FabricUIManagerBinding::setConstraints(
    jint surfaceId,
    jfloat minWidth,
    jfloat maxWidth,
    jfloat minHeight,
    jfloat maxHeight,
    jfloat offsetX,
    jfloat offsetY,
    jboolean isRTL,
    jboolean doLeftAndRightSwapInRTL) {
  TraceSection s("FabricUIManagerBinding::setConstraints");

  auto scheduler = getScheduler();
  if (!scheduler) {
    LOG(ERROR)
        << "FabricUIManagerBinding::setConstraints: scheduler disappeared";
    return;
  }

  auto minimumSize = Size{
      .width = minWidth / pointScaleFactor_,
      .height = minHeight / pointScaleFactor_};
  auto maximumSize = Size{
      .width = maxWidth / pointScaleFactor_,
      .height = maxHeight / pointScaleFactor_};

  LayoutContext context;
  context.viewportOffset =
      Point{.x = offsetX / pointScaleFactor_, .y = offsetY / pointScaleFactor_};
  context.pointScaleFactor = {pointScaleFactor_};
  context.swapLeftAndRightInRTL = doLeftAndRightSwapInRTL != 0;
  LayoutConstraints constraints = {};
  constraints.minimumSize = minimumSize;
  constraints.maximumSize = maximumSize;
  constraints.layoutDirection = (isRTL != 0u) ? LayoutDirection::RightToLeft
                                              : LayoutDirection::LeftToRight;

#if LOOM_RENDERER
  LoomBinding::setSurfaceConstraints(
      surfaceId,
      minimumSize.width,
      minimumSize.height,
      maximumSize.width,
      maximumSize.height,
      isRTL != 0,
      context.viewportOffset.x,
      context.viewportOffset.y);
#endif

  {
    std::shared_lock lock(surfaceHandlerRegistryMutex_);
    auto iterator = surfaceHandlerRegistry_.find(surfaceId);
    if (iterator == surfaceHandlerRegistry_.end()) {
      LOG(ERROR)
          << "FabricUIManagerBinding::setConstraints: Surface with given id is not found";
      return;
    }
    auto* surfaceHandler = std::get_if<SurfaceHandler>(&iterator->second);
    if (surfaceHandler != nullptr) {
      surfaceHandler->constraintLayout(constraints, context);
    }
  }
}

#pragma mark - Install/uninstall java binding

void FabricUIManagerBinding::installFabricUIManager(
    jni::alias_ref<JRuntimeExecutor::javaobject> runtimeExecutorHolder,
    jni::alias_ref<JRuntimeScheduler::javaobject> runtimeSchedulerHolder,
    jni::alias_ref<JFabricUIManager::javaobject> javaUIManager,
    EventBeatManager* eventBeatManager,
    ComponentFactory* componentsRegistry) {
  TraceSection s("FabricUIManagerBinding::installFabricUIManager");

  enableFabricLogs_ = ReactNativeFeatureFlags::enableFabricLogs();

  if (enableFabricLogs_) {
    LOG(WARNING)
        << "FabricUIManagerBinding::installFabricUIManager() was called (address: "
        << this << ").";
  }

  std::unique_lock lock(installMutex_);

  auto globalJavaUiManager = make_global(javaUIManager);
  mountingManager_ =
      std::make_shared<FabricMountingManager>(globalJavaUiManager);

  std::shared_ptr<const ContextContainer> contextContainer =
      std::make_shared<ContextContainer>();

  auto runtimeExecutor = runtimeExecutorHolder->cthis()->get();

  auto runtimeScheduler = runtimeSchedulerHolder->cthis()->get().lock();
  if (runtimeScheduler) {
    runtimeExecutor =
        [runtimeScheduler](
            std::function<void(jsi::Runtime & runtime)>&& callback) {
          runtimeScheduler->scheduleWork(std::move(callback));
        };
    contextContainer->insert(
        RuntimeSchedulerKey, std::weak_ptr<RuntimeScheduler>(runtimeScheduler));
  }

  EventBeat::Factory eventBeatFactory =
      [eventBeatManager, &runtimeScheduler, globalJavaUiManager](
          std::shared_ptr<EventBeat::OwnerBox> ownerBox)
      -> std::unique_ptr<EventBeat> {
    return std::make_unique<AndroidEventBeat>(
        std::move(ownerBox),
        eventBeatManager,
        *runtimeScheduler,
        globalJavaUiManager);
  };

  auto fabricUIManagerOwner =
      std::make_shared<jni::global_ref<jobject>>(globalJavaUiManager);
  // The alias and its owner must stay registered for the container's lifetime.
  contextContainer->insert("FabricUIManagerOwner", fabricUIManagerOwner);
  contextContainer->insert(
      "FabricUIManager", jni::alias_ref<jobject>(*fabricUIManagerOwner));

  animationChoreographer_ =
      std::make_shared<LoomAnimationChoreographer>(globalJavaUiManager);

  auto toolbox = SchedulerToolbox{};
  toolbox.contextContainer = contextContainer;
  toolbox.componentRegistryFactory = componentsRegistry->buildRegistryFunction;

  // TODO: (T132338609) runtimeExecutor should execute lambdas after
  // main bundle eval, and bindingsInstallExecutor should execute before.
  toolbox.bridgelessBindingsExecutor = std::nullopt;
  toolbox.runtimeExecutor = runtimeExecutor;

  toolbox.eventBeatFactory = eventBeatFactory;

  toolbox.animationChoreographer = animationChoreographer_;

#if LOOM_RENDERER
  // Registered before the scheduler exists: the Loom binding reads its host
  // from the context container when the JS runtime installs it.
  loomHost_ = std::make_shared<LoomAndroidHost>(this, contextContainer);
  LoomHost::registerIn(*contextContainer, loomHost_);
#endif
#if LOOM_RENDERER
  scheduler_ = std::make_shared<Scheduler>(toolbox, nullptr, this);
#else
  animationDriver_ = std::make_shared<LayoutAnimationDriver>(
      runtimeExecutor, contextContainer, this);
  scheduler_ =
      std::make_shared<Scheduler>(toolbox, animationDriver_.get(), this);
#endif
#if LOOM_RENDERER
  directTextLayoutManager_ =
      std::make_shared<const TextLayoutManager>(contextContainer);
  eventManager_ = std::make_shared<LoomEventManager>(scheduler_->getEventDispatcher());
  // Off by default: parallelText on a low-end phone (Infinix X6516, 8 slow
  // cores) competed with the UI thread's early view creation and lost more
  // than it won; `Loom.parallelText = true` or `setprop debug.loom.paralleltext 1`
  // turns it on.
  LoomAndroidHost::loadSettings(globalJavaUiManager);
  if (LoomSettings::enabled(LoomSettingId::ParallelText)) {
    LoomAndroidHost::warmUpTextPool(); // threads up before the first surface measures
  }
#endif
}

void FabricUIManagerBinding::uninstallFabricUIManager() {
  if (enableFabricLogs_) {
    LOG(WARNING)
        << "FabricUIManagerBinding::uninstallFabricUIManager() was called (address: "
        << this << ").";
  }

#if LOOM_RENDERER
  // Join before taking installMutex_: text computation takes its shared side.
  auto scheduler = getScheduler();
  if (scheduler) LoomBinding::shutdownWorkerForUIManager(*scheduler->getUIManager());
#endif
  std::unique_lock lock(installMutex_);
  animationDriver_ = nullptr;
#if LOOM_RENDERER
  if (loomHost_) {
    loomHost_->invalidate();
    loomHost_ = nullptr;
  }
  eventManager_ = nullptr;
  directTextLayoutManager_ = nullptr;
#endif
  scheduler_ = nullptr;
  animationChoreographer_ = nullptr;
  mountingManager_ = nullptr;
}

std::shared_ptr<FabricMountingManager>
FabricUIManagerBinding::getMountingManager(const char* locationHint) {
  std::shared_lock lock(installMutex_);
  if (!mountingManager_) {
    LOG(ERROR) << "FabricMountingManager::" << locationHint
               << " mounting manager disappeared";
  }
  // Need to return a copy of the shared_ptr to make sure this is safe if called
  // concurrently with uninstallFabricUIManager
  return mountingManager_;
}

#if LOOM_RENDERER
std::function<void(std::function<void()>)> FabricUIManagerBinding::loomGetUIExecutor() {
  return [](std::function<void()> callback) {
    jni::ThreadScope::WithClassLoader([callback = std::move(callback)]() mutable {
      static auto schedule = JFabricUIManager::javaClassStatic()->getStaticMethod<
          void(jni::alias_ref<jni::JRunnable::javaobject>)>("loomDirectScheduleResult");
      schedule(JFabricUIManager::javaClassStatic(), jni::JNativeRunnable::newObjectCxxArgs(std::move(callback)));
    });
  };
}

bool FabricUIManagerBinding::loomCanMeasureTextOnWorker() {
  auto manager = getMountingManager("directCanMeasureTextOnWorker");
  return manager && manager->directCanMeasureTextOnWorker();
}

void FabricUIManagerBinding::loomApplyBatch(
    SurfaceId surfaceId, LoomMountBatch operations) {
  if (auto manager = getMountingManager("loomApplyBatch")) {
    manager->directApplyBatch(surfaceId, std::move(operations));
  }
}

void FabricUIManagerBinding::loomFailSurface(
    SurfaceId surfaceId, const std::string& reason) {
  if (auto manager = getMountingManager("loomFailSurface")) {
    manager->directFailSurface(surfaceId, reason);
  }
}

void FabricUIManagerBinding::loomRegisterEventTarget(
    SurfaceId surfaceId, Tag tag, InstanceHandle::Shared instanceHandle) {
  std::shared_ptr<LoomEventManager> eventManager;
  {
    std::shared_lock lock(installMutex_);
    eventManager = eventManager_;
  }
  if (eventManager)
    eventManager->registerTarget(surfaceId, tag, std::move(instanceHandle));
}

void FabricUIManagerBinding::loomUnregisterEventTarget(
    SurfaceId surfaceId, Tag tag) {
  std::shared_ptr<LoomEventManager> eventManager;
  {
    std::shared_lock lock(installMutex_);
    eventManager = eventManager_;
  }
  if (eventManager)
    eventManager->unregisterTarget(surfaceId, tag);
}

void FabricUIManagerBinding::loomDispatchEvent(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& eventName,
    folly::dynamic payload,
    RawEvent::Category category,
    bool unique) {
  std::shared_ptr<Scheduler> scheduler;
  std::shared_ptr<LoomEventManager> eventManager;
  {
    std::shared_lock lock(installMutex_);
    scheduler = scheduler_;
    eventManager = eventManager_;
  }
  // Dispatch can synchronously induce Android's event beat, which enters Java
  // and may run JS that re-enters or tears down Fabric. Retain the owners, but
  // release installMutex_ before invoking that callback chain.
  if (scheduler && eventManager)
    eventManager->dispatch(
        surfaceId, tag, eventName, std::move(payload), category, unique);
}

std::optional<Size> FabricUIManagerBinding::loomMeasureText(
    SurfaceId surfaceId,
    const AttributedString& attributedString,
    const ParagraphAttributes& paragraphAttributes,
    const Size& minimumSize,
    const Size& maximumSize) {
  // On Android this round-trips into `FabricUIManager.measureText`, the same
  // call the shadow-tree path makes, with the same caches behind it.
  std::shared_ptr<const TextLayoutManager> manager;
  {
    std::shared_lock lock(installMutex_);
    manager = directTextLayoutManager_;
  }
  if (!manager) {
    return std::nullopt;
  }

  TextLayoutContext layoutContext;
  layoutContext.pointScaleFactor = pointScaleFactor_;

  LayoutConstraints layoutConstraints;
  layoutConstraints.minimumSize = minimumSize;
  layoutConstraints.maximumSize = maximumSize;

  auto measurement = manager->measure(
      AttributedStringBox{attributedString},
      paragraphAttributes,
      layoutContext,
      layoutConstraints);
  return measurement.size;
}

LoomTextMeasurement FabricUIManagerBinding::loomPrepareText(
    SurfaceId surfaceId,
    const AttributedString& attributedString,
    const ParagraphAttributes& paragraphAttributes,
    const Size& minimumSize,
    const Size& maximumSize) {
  try {
    LoomTextMeasurement result;
    std::shared_ptr<const TextLayoutManager> manager;
    {
      std::shared_lock lock(installMutex_);
      manager = directTextLayoutManager_;
    }
    if (!manager) {
      return result;
    }
    TextLayoutContext layoutContext;
    layoutContext.pointScaleFactor = pointScaleFactor_;
    layoutContext.surfaceId = surfaceId;
    LayoutConstraints constraints;
    constraints.minimumSize = minimumSize;
    constraints.maximumSize = maximumSize;
  #if LOOM_PROFILE
    TextLayoutManagerProfile profile;
    auto* profileOutput = &profile;
  #else
    TextLayoutManagerProfile* profileOutput = nullptr;
  #endif
    auto prepared = std::make_shared<TextLayoutManager::PreparedTextLayout>(
        manager->prepareLayout(
            attributedString, paragraphAttributes, layoutContext, constraints, profileOutput));
    const bool hasAttachments = std::any_of(
        attributedString.getFragments().begin(),
        attributedString.getFragments().end(),
        [](const auto& fragment) { return fragment.isAttachment(); });
    auto measurement = hasAttachments
        ? manager->measurePreparedLayout(*prepared, layoutContext, constraints)
        : manager->measurePreparedLayoutSize(
              *prepared, layoutContext, constraints, profileOutput);
    result.size = measurement.size;
    result.minimumSize = minimumSize;
    result.maximumSize = maximumSize;
    result.pointScaleFactor = layoutContext.pointScaleFactor;
    result.valid = true;
    result.preparedLayout = std::move(prepared);
  #if LOOM_PROFILE
    result.profile = {
        .spannableBuildNanos = profile.spannableBuildNanos,
        .paintSetupNanos = profile.paintSetupNanos,
        .layoutCreateNanos = profile.layoutCreateNanos,
        .isBoringNanos = profile.isBoringNanos,
        .boringMakeNanos = profile.boringMakeNanos,
        .measurementResultNanos = profile.measurementResultNanos,
        .layoutKind = profile.layoutKind};
    result.profile.simplePlainTextFastPath = profile.simplePlainTextFastPath;
  #endif
    return result;
  } catch (const jni::JniException& error) {
    logDirectRendererJavaException("loomPrepareText", error);
    throw;
  }
}

LoomTextMeasurement FabricUIManagerBinding::loomPrepareTextForMount(
    SurfaceId surfaceId,
    const AttributedString& attributedString,
    const ParagraphAttributes& paragraphAttributes,
    const Size& minimumSize,
    const Size& maximumSize) {
  try {
    LoomTextMeasurement result;
    std::shared_ptr<const TextLayoutManager> manager;
    {
      std::shared_lock lock(installMutex_);
      manager = directTextLayoutManager_;
    }
    if (!manager) {
      return result;
    }

    TextLayoutContext layoutContext;
    layoutContext.pointScaleFactor = pointScaleFactor_;
    layoutContext.surfaceId = surfaceId;
    LayoutConstraints constraints;
    constraints.minimumSize = minimumSize;
    constraints.maximumSize = maximumSize;
  #if LOOM_PROFILE
    TextLayoutManagerProfile profile;
    auto* profileOutput = &profile;
  #else
    TextLayoutManagerProfile* profileOutput = nullptr;
  #endif
    auto prepared = manager->prepareLayout(
        attributedString,
        paragraphAttributes,
        layoutContext,
        constraints,
        profileOutput);
    result.minimumSize = minimumSize;
    result.maximumSize = maximumSize;
    result.pointScaleFactor = layoutContext.pointScaleFactor;
    result.valid = true;
    result.preparedLayout = std::make_shared<TextLayoutManager::PreparedTextLayout>(
        std::move(prepared));
  #if LOOM_PROFILE
    result.profile = {
        .paintSetupNanos = profile.paintSetupNanos,
        .isBoringNanos = profile.isBoringNanos,
        .boringMakeNanos = profile.boringMakeNanos,
        .layoutKind = profile.layoutKind};
  #endif
    return result;
  } catch (const jni::JniException& error) {
    logDirectRendererJavaException("loomPrepareTextForMount", error);
    throw;
  }
}

std::pair<float, float> FabricUIManagerBinding::loomGetScrollOffset(
    SurfaceId surfaceId,
    Tag tag) {
  if (auto manager = getMountingManager("loomGetScrollOffset")) {
    return manager->directGetScrollOffset(surfaceId, tag);
  }
  return {0, 0};
}

std::optional<std::pair<float, float>>
FabricUIManagerBinding::loomGetViewLocationInSurface(
    SurfaceId surfaceId,
    Tag tag) {
  if (auto manager = getMountingManager("loomGetViewLocationInSurface")) {
    return manager->directGetViewLocationInSurface(surfaceId, tag);
  }
  return std::nullopt;
}

void FabricUIManagerBinding::loomReportUnsupportedInlineAttachment(
    SurfaceId surfaceId,
    Tag tag) {
  LOG(ERROR) << "LOOM_RENDERER does not support inline native text attachment "
             << tag << " on surface " << surfaceId
             << "; use the standard Fabric renderer for this tree";
}

void FabricUIManagerBinding::loomUpdateState(
    const LoomTextStateUpdate& update) {
  if (auto manager = getMountingManager("loomUpdateState")) {
    manager->directUpdateState(update);
  }
}

void FabricUIManagerBinding::loomUpdateComponentState(
    SurfaceId surfaceId,
    Tag tag,
    const std::shared_ptr<const State>& state) {
  if (auto manager = getMountingManager("loomUpdateComponentState")) {
    manager->directUpdateComponentState(surfaceId, tag, state);
  }
}

void FabricUIManagerBinding::loomDispatchCommand(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& commandName,
    const folly::dynamic& args) {
  if (auto manager = getMountingManager("loomDispatchCommand")) {
    // Reuses the stock Fabric entry point, which queues a command mount item
    // for the host to run; the Loom renderer deliberately does not
    // reimplement command routing.
    manager->dispatchCommand(surfaceId, tag, commandName, args);
  }
}

void FabricUIManagerBinding::loomSendAccessibilityEvent(
    SurfaceId surfaceId, Tag tag, const std::string& eventType) {
  if (auto manager =
          getMountingManager("loomSendAccessibilityEvent")) {
    manager->sendAccessibilityEvent(surfaceId, tag, eventType);
  }
}

void FabricUIManagerBinding::loomSetIsJSResponder(
    SurfaceId surfaceId, Tag tag, bool isJSResponder, bool blockNativeResponder) {
  if (auto manager = getMountingManager("loomSetIsJSResponder")) {
    manager->setIsJSResponder(
        surfaceId, tag, isJSResponder, blockNativeResponder);
  }
}

void FabricUIManagerBinding::dispatchDirectEvent(
    jint surfaceId,
    jint reactTag,
    jni::alias_ref<jstring> eventName,
    jboolean canCoalesce,
    ReadableNativeMap* params,
    jint eventCategory,
    jlong eventTimestamp,
    jboolean synchronous) {
  std::shared_ptr<Scheduler> scheduler;
  std::shared_ptr<LoomEventManager> eventManager;
  {
    std::shared_lock lock(installMutex_);
    scheduler = scheduler_;
    eventManager = eventManager_;
  }
  if (!scheduler || !eventManager)
    return;
  auto payload = params ? params->consume() : folly::dynamic::object();
  // A synchronous event beat can call into JS before dispatch returns. Keep
  // the Scheduler alive, and never hold installMutex_ across that reentrancy.
  eventManager->dispatch(
      static_cast<SurfaceId>(surfaceId),
      static_cast<Tag>(reactTag),
      eventName->toStdString(),
      std::move(payload),
      static_cast<RawEvent::Category>(eventCategory),
      canCoalesce != 0,
      HighResTimeStamp::fromDOMHighResTimeStamp(eventTimestamp),
      synchronous != 0);
}
#endif

void FabricUIManagerBinding::schedulerDidFinishTransaction(
    const std::shared_ptr<const MountingCoordinator>& mountingCoordinator) {
#if LOOM_RENDERER
  // The direct JS host binding owns rendering when enabled. The legacy
  // transaction path must not diff, pull, queue, or mount anything.
  (void)mountingCoordinator;
  return;
#else
  if (ReactNativeFeatureFlags::enableAccumulatedUpdatesInRawPropsAndroid()) {
    // We don't do anything here. We will pull the transaction in
    // `schedulerShouldRenderTransactions`.
  } else {
    // We shouldn't be pulling the transaction here (which triggers diffing of
    // the trees to determine the mutations to run on the host platform),
    // but we have to due to current limitations in the Android implementation.
    auto mountingTransaction = mountingCoordinator->pullTransaction(
        /* willPerformAsynchronously = */ true);
    if (!mountingTransaction.has_value()) {
      return;
    }

    std::unique_lock<std::mutex> lock(pendingTransactionsMutex_);
    auto pendingTransaction = std::find_if(
        pendingTransactions_.begin(),
        pendingTransactions_.end(),
        [&](const auto& transaction) {
          return transaction.getSurfaceId() ==
              mountingTransaction->getSurfaceId();
        });

    if (pendingTransaction != pendingTransactions_.end()) {
      pendingTransaction->mergeWith(std::move(*mountingTransaction));
    } else {
      pendingTransactions_.push_back(std::move(*mountingTransaction));
    }
  }
#endif
}

void FabricUIManagerBinding::schedulerShouldRenderTransactions(
    const std::shared_ptr<const MountingCoordinator>& mountingCoordinator) {
#if LOOM_RENDERER
  (void)mountingCoordinator;
  return;
#else
  auto mountingManager =
      getMountingManager("schedulerShouldRenderTransactions");
  if (!mountingManager) {
    return;
  }

  if (ReactNativeFeatureFlags::enableImagePrefetchingAndroid()) {
    auto weakImageFetcher =
        scheduler_->getContextContainer()->find<std::weak_ptr<ImageFetcher>>(
            ImageFetcherKey);
    auto imageFetcher = weakImageFetcher.has_value()
        ? weakImageFetcher.value().lock()
        : nullptr;
    if (imageFetcher != nullptr) {
      imageFetcher->flushImageRequests();
    }
  }

  if (ReactNativeFeatureFlags::enableMountingCoordinatorPullModelAndroid()) {
    // Pull model: do NOT pull the transaction or build the batch here (on the
    // commit thread). Just notify Java that a transaction is available; the UI
    // thread calls back into `pullAndExecuteTransaction` at its next mount
    // dispatch.
    auto surfaceId = mountingCoordinator->getSurfaceId();
    {
      std::scoped_lock lock(pullableMountingCoordinatorsMutex_);
      auto& pullable = pullableMountingCoordinators_[surfaceId];
      if (pullable != nullptr && pullable != mountingCoordinator) {
        LOG(WARNING)
            << "FabricUIManagerBinding::schedulerShouldRenderTransactions: "
            << "surface " << surfaceId
            << " was restarted while the previous surface's commit waits to be pulled";
      }
      pullable = mountingCoordinator;
    }
    mountingManager->onTransactionAvailable(surfaceId);
  } else if (ReactNativeFeatureFlags::
                 enableAccumulatedUpdatesInRawPropsAndroid()) {
    auto mountingTransaction = mountingCoordinator->pullTransaction(
        /* willPerformAsynchronously = */ true);
    if (mountingTransaction.has_value()) {
      auto transaction = std::move(*mountingTransaction);
      mountingManager->executeMount(transaction);
    }
  } else {
    std::vector<MountingTransaction> pendingTransactions;

    {
      // Retain the lock to access the pending transactions but not to execute
      // the mount operations because that method can call into this method
      // again.
      //
      // This can be re-entrant when mounting manager triggers state updates
      // synchronously (this can happen when committing from the UI thread).
      // This is safe because we're already combining all the transactions for
      // the same surface ID in a single transaction in the pending transactions
      // list, so operations won't run out of order.
      std::unique_lock<std::mutex> lock(pendingTransactionsMutex_);
      pendingTransactions_.swap(pendingTransactions);
    }

    for (auto& transaction : pendingTransactions) {
      mountingManager->executeMount(transaction);
    }
  }
#endif
}

void FabricUIManagerBinding::schedulerShouldMergeReactRevision(
    SurfaceId surfaceId) {
  if (ReactNativeFeatureFlags::enableFabricCommitBranchingMergeOnMainThread()) {
    auto mountingManager =
        getMountingManager("schedulerShouldMergeReactRevision");
    if (mountingManager) {
      mountingManager->scheduleReactRevisionMerge(surfaceId);
    }
  } else {
    mergeReactRevision(surfaceId);
  }
}

void FabricUIManagerBinding::mergeReactRevision(SurfaceId surfaceId) {
  auto scheduler = getScheduler();
  if (!scheduler) {
    return;
  }

  scheduler->getUIManager()->getShadowTreeRegistry().visit(
      surfaceId,
      [](const ShadowTree& shadowTree) { shadowTree.mergeReactRevision(); });
}

void FabricUIManagerBinding::schedulerDidRequestPreliminaryViewAllocation(
    const ShadowNode& shadowNode) {
  using namespace std::literals::string_view_literals;

  if (ReactNativeFeatureFlags::disableViewPreallocationAndroid()) {
    return;
  }

  if (ReactNativeFeatureFlags::disableImageViewPreallocationAndroid() &&
      std::string_view(shadowNode.getComponentName()) == "Image"sv) {
    return;
  }

  auto mountingManager = getMountingManager("preallocateView");
  if (!mountingManager) {
    return;
  }
  const auto wasPreallocated =
      mountingManager->maybePreallocateShadowNode(shadowNode);
  // Only the Views of ShadowNode that were pre-allocated (forms views) need
  // to be destroyed if the ShadowNode is destroyed and no mount created its
  // View. Loom: skip the callback when nothing was preallocated (rendering on
  // the main thread), which would otherwise cost a JNI call per destroyed node.
  if (wasPreallocated &&
      shadowNode.getTraits().check(ShadowNodeTraits::Trait::FormsView)) {
    shadowNode.getFamilyShared()->onFamilyDestroyed(
        [weakMountingManager =
             std::weak_ptr(mountingManager)](const ShadowNodeFamily& family) {
          if (auto mountingManager = weakMountingManager.lock()) {
            mountingManager->destroyUnmountedShadowNode(family);
          }
        });
  }
}

void FabricUIManagerBinding::schedulerDidDispatchCommand(
    const ShadowView& shadowView,
    const std::string& commandName,
    const folly::dynamic& args) {
  auto mountingManager = getMountingManager("schedulerDidDispatchCommand");
  if (!mountingManager) {
    return;
  }
  mountingManager->dispatchCommand(
      shadowView.surfaceId, shadowView.tag, commandName, args);
}

void FabricUIManagerBinding::schedulerDidSendAccessibilityEvent(
    const ShadowView& shadowView,
    const std::string& eventType) {
  auto mountingManager =
      getMountingManager("schedulerDidSendAccessibilityEvent");
  if (!mountingManager) {
    return;
  }
  mountingManager->sendAccessibilityEvent(
      shadowView.surfaceId, shadowView.tag, eventType);
}

void FabricUIManagerBinding::schedulerDidSetIsJSResponder(
    const ShadowView& shadowView,
    bool isJSResponder,
    bool blockNativeResponder) {
  auto mountingManager = getMountingManager("schedulerDidSetIsJSResponder");
  if (!mountingManager) {
    return;
  }
  mountingManager->setIsJSResponder(
      shadowView.surfaceId, shadowView.tag, isJSResponder, blockNativeResponder);
}

void FabricUIManagerBinding::schedulerShouldSynchronouslyUpdateViewOnUIThread(
    Tag tag,
    const folly::dynamic& props) {
  if ((LOOM_RENDERER || ReactNativeFeatureFlags::cxxNativeAnimatedEnabled()) && mountingManager_) {
    mountingManager_->synchronouslyUpdateViewOnUIThread(tag, props);
  }
}

void FabricUIManagerBinding::schedulerShouldSynchronouslyUpdateAnimatedProps(
    const std::unordered_map<Tag, AnimatedProps>& updates) {
  if (ReactNativeFeatureFlags::cxxNativeAnimatedEnabled() && mountingManager_) {
    mountingManager_->synchronouslyUpdateAnimatedProps(updates);
  }
}

void FabricUIManagerBinding::schedulerDidUpdateShadowTree(
    const std::unordered_map<Tag, folly::dynamic>& /*tagToProps*/) {
  // no-op
}

void FabricUIManagerBinding::schedulerDidCaptureViewSnapshot(
    Tag tag,
    SurfaceId surfaceId) {
  if (mountingManager_) {
    mountingManager_->captureViewSnapshot(tag, surfaceId);
  }
}

void FabricUIManagerBinding::schedulerDidSetViewSnapshot(
    Tag sourceTag,
    Tag targetTag,
    SurfaceId surfaceId) {
  if (mountingManager_) {
    mountingManager_->setViewSnapshot(sourceTag, targetTag, surfaceId);
  }
}

void FabricUIManagerBinding::schedulerDidClearPendingSnapshots() {
  if (mountingManager_) {
    mountingManager_->clearPendingSnapshots();
  }
}

void FabricUIManagerBinding::onAnimationStarted() {
  auto mountingManager = getMountingManager("onAnimationStarted");
  if (!mountingManager) {
    return;
  }
  mountingManager->onAnimationStarted();
}

void FabricUIManagerBinding::onAllAnimationsComplete() {
  auto mountingManager = getMountingManager("onAnimationComplete");
  if (!mountingManager) {
    return;
  }
  mountingManager->onAllAnimationsComplete();
}

void FabricUIManagerBinding::recordDirectNativeProps(jint tag, NativeMap* props) {
#if LOOM_RENDERER
  LoomBinding::recordNativeProps(tag, props->consume());
#else
  (void)tag;
  (void)props;
#endif
}

#if LOOM_RENDERER
namespace {
// Must match FabricUIManager.encodeAnimatedPaint. Only the Loom renderer decodes these.
constexpr const char* kPaintDoubleKeys[] = {
    "opacity", "elevation", "shadowOpacity", "shadowRadius"};
constexpr const char* kPaintIntKeys[] = {
    "backgroundColor", "tintColor", "overlayColor", "borderColor",
    "borderTopColor", "borderBottomColor", "borderLeftColor",
    "borderRightColor", "shadowColor"};
constexpr const char* kTransformOps[] = {
    "translateX", "translateY", "scale", "scaleX", "scaleY", "rotateX",
    "rotateY", "rotateZ", "skewX", "skewY", "perspective"};
constexpr int kTransformKey = 100;
constexpr int kIntKeyBase = 10;
} // namespace
#endif

void FabricUIManagerBinding::recordDirectPaintBatch(
    jni::alias_ref<jni::JArrayDouble> data,
    jint length) {
#if LOOM_RENDERER
  if (length <= 0) {
    return;
  }
  thread_local std::vector<double> values;
  values.resize(static_cast<size_t>(length));
  data->getRegion(0, length, values.data());
  int i = 0;
  while (i + 1 < length) {
    const auto tag = static_cast<Tag>(values[i]);
    const int end = std::min<int>(length, i + 2 + static_cast<int>(values[i + 1]));
    i += 2;
    folly::dynamic paint = folly::dynamic::object();
    while (i + 1 < end) {
      const int key = static_cast<int>(values[i]);
      if (key == kTransformKey) {
        const int n = static_cast<int>(values[i + 1]);
        i += 2;
        folly::dynamic ops = folly::dynamic::array();
        for (int j = 0; j < n && i + 1 < end; ++j, i += 2) {
          const int op = static_cast<int>(values[i]);
          if (op < 0 || op >= static_cast<int>(sizeof(kTransformOps) / sizeof(kTransformOps[0]))) {
            continue;
          }
          ops.push_back(folly::dynamic::object(kTransformOps[op], values[i + 1]));
        }
        paint["transform"] = std::move(ops);
      } else if (key >= kIntKeyBase) {
        const int index = key - kIntKeyBase;
        if (index < static_cast<int>(sizeof(kPaintIntKeys) / sizeof(kPaintIntKeys[0]))) {
          paint[kPaintIntKeys[index]] = static_cast<int64_t>(values[i + 1]);
        }
        i += 2;
      } else {
        if (key >= 0 && key < static_cast<int>(sizeof(kPaintDoubleKeys) / sizeof(kPaintDoubleKeys[0]))) {
          paint[kPaintDoubleKeys[key]] = values[i + 1];
        }
        i += 2;
      }
    }
    i = end;
    LoomBinding::recordNativePaintProps(tag, std::move(paint));
  }
#else
  (void)data;
  (void)length;
#endif
}

void FabricUIManagerBinding::flushDirectAnimationFrame() {
#if LOOM_RENDERER
  LoomBinding::flushAnimationFrames();
#endif
}

void FabricUIManagerBinding::registerNatives() {
#if LOOM_RENDERER
  // Before any runtime exists: the core reads the traits from its first use.
  LoomPlatformTraits::install(LoomAndroidHost::platformTraits());
#endif
  registerHybrid({
      makeNativeMethod("initHybrid", FabricUIManagerBinding::initHybrid),
      makeNativeMethod("recordDirectNativeProps", FabricUIManagerBinding::recordDirectNativeProps),
      makeNativeMethod(
          "recordDirectPaintBatch",
          FabricUIManagerBinding::recordDirectPaintBatch),
      makeNativeMethod(
          "flushDirectAnimationFrame",
          FabricUIManagerBinding::flushDirectAnimationFrame),
      makeNativeMethod(
          "installFabricUIManager",
          FabricUIManagerBinding::installFabricUIManager),
      makeNativeMethod("startSurface", FabricUIManagerBinding::startSurface),
      makeNativeMethod(
          "startSurfaceWithConstraints",
          FabricUIManagerBinding::startSurfaceWithConstraints),
      makeNativeMethod("stopSurface", FabricUIManagerBinding::stopSurface),
      makeNativeMethod(
          "setConstraints", FabricUIManagerBinding::setConstraints),
      makeNativeMethod(
          "setPixelDensity", FabricUIManagerBinding::setPixelDensity),
      makeNativeMethod(
          "driveCxxAnimations", FabricUIManagerBinding::driveCxxAnimations),
      makeNativeMethod(
          "driveAnimationBackend",
          FabricUIManagerBinding::driveAnimationBackend),
      makeNativeMethod(
          "drainPreallocateViewsQueue",
          FabricUIManagerBinding::drainPreallocateViewsQueue),
      makeNativeMethod("reportMount", FabricUIManagerBinding::reportMount),
#if LOOM_RENDERER
      makeNativeMethod(
          "dispatchDirectEvent", FabricUIManagerBinding::dispatchDirectEvent),
#endif
      makeNativeMethod(
          "pullAndExecuteTransaction",
          FabricUIManagerBinding::pullAndExecuteTransaction),
      makeNativeMethod(
          "uninstallFabricUIManager",
          FabricUIManagerBinding::uninstallFabricUIManager),
      makeNativeMethod(
          "startSurfaceWithSurfaceHandler",
          FabricUIManagerBinding::startSurfaceWithSurfaceHandler),
      makeNativeMethod(
          "stopSurfaceWithSurfaceHandler",
          FabricUIManagerBinding::stopSurfaceWithSurfaceHandler),
      makeNativeMethod(
          "findNextFocusableElement",
          FabricUIManagerBinding::findNextFocusableElement),
      makeNativeMethod(
          "getRelativeAncestorList",
          FabricUIManagerBinding::getRelativeAncestorList),
      makeNativeMethod(
          "mergeReactRevision", FabricUIManagerBinding::mergeReactRevision),
  });
}

} // namespace facebook::react
