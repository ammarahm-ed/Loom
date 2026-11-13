/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <fbjni/fbjni.h>
#include <react/jni/JRuntimeExecutor.h>
#include <react/jni/JRuntimeScheduler.h>
#include <react/jni/ReadableNativeMap.h>
#include <react/renderer/scheduler/SchedulerDelegate.h>
#include <react/renderer/scheduler/SurfaceHandler.h>
#include <react/renderer/uimanager/LayoutAnimationStatusDelegate.h>
#include <react/renderer/uimanager/primitives.h>
#if LOOM_RENDERER
#include <react/renderer/uimanager/LoomRenderer.h>
#endif

#include "AndroidAnimationChoreographer.h"
#include "JFabricUIManager.h"
#include "SurfaceHandlerBinding.h"

namespace facebook::react {

class ComponentFactory;
class EventBeatManager;
class FabricMountingManager;
class Instance;
class LayoutAnimationDriver;
class Scheduler;
#if LOOM_RENDERER
class TextLayoutManager;
#endif

class FabricUIManagerBinding : public jni::HybridClass<FabricUIManagerBinding>,
                               public SchedulerDelegate,
                               public LayoutAnimationStatusDelegate {
 public:
  constexpr static const char *const kJavaDescriptor = "Lcom/facebook/react/fabric/FabricUIManagerBinding;";

  static void registerNatives();

  // Must be kept public even though it is not used by any other class in React
  // Native. Used by 3rd party libraries, for example Reanimated:
  // https://github.com/software-mansion/react-native-reanimated/
  std::shared_ptr<Scheduler> getScheduler();

 private:
  void setConstraints(
      jint surfaceId,
      jfloat minWidth,
      jfloat maxWidth,
      jfloat minHeight,
      jfloat maxHeight,
      jfloat offsetX,
      jfloat offsetY,
      jboolean isRTL,
      jboolean doLeftAndRightSwapInRTL);

  static void initHybrid(jni::alias_ref<jhybridobject> jobj);

  void installFabricUIManager(
      jni::alias_ref<JRuntimeExecutor::javaobject> runtimeExecutorHolder,
      jni::alias_ref<JRuntimeScheduler::javaobject> runtimeSchedulerHolder,
      jni::alias_ref<JFabricUIManager::javaobject> javaUIManager,
      EventBeatManager *eventBeatManager,
      ComponentFactory *componentsRegistry);

  void startSurface(jint surfaceId, jni::alias_ref<jstring> moduleName, NativeMap *initialProps);

  void startSurfaceWithConstraints(
      jint surfaceId,
      jni::alias_ref<jstring> moduleName,
      NativeMap *initialProps,
      jfloat minWidth,
      jfloat maxWidth,
      jfloat minHeight,
      jfloat maxHeight,
      jfloat offsetX,
      jfloat offsetY,
      jboolean isRTL,
      jboolean doLeftAndRightSwapInRTL);

  void stopSurface(jint surfaceId);

  void startSurfaceWithSurfaceHandler(
      jint surfaceId,
      jni::alias_ref<SurfaceHandlerBinding::jhybridobject> surfaceHandlerBinding,
      jboolean isMountable);

  void stopSurfaceWithSurfaceHandler(jni::alias_ref<SurfaceHandlerBinding::jhybridobject> surfaceHandler);

  void schedulerDidFinishTransaction(const std::shared_ptr<const MountingCoordinator> &mountingCoordinator) override;

  void schedulerShouldRenderTransactions(
      const std::shared_ptr<const MountingCoordinator> &mountingCoordinator) override;
#if LOOM_RENDERER
  void loomSetConstraints(
      SurfaceId,
      float minWidth,
      float minHeight,
      float maxWidth,
      float maxHeight,
      bool isRTL,
      float viewportOffsetX,
      float viewportOffsetY) override;
  std::function<void(std::function<void()>)> loomGetUIExecutor() override;
  bool loomCanMeasureTextOnWorker() override;
  void loomApplyBatch(SurfaceId, LoomMountBatch) override;
  void loomFailSurface(SurfaceId, const std::string&) override;
  void loomRegisterEventTarget(SurfaceId, Tag, InstanceHandle::Shared) override;
  void loomUnregisterEventTarget(SurfaceId, Tag) override;
  void loomDispatchEvent(
      SurfaceId, Tag, const std::string&, folly::dynamic, RawEvent::Category, bool) override;
  std::optional<Size> loomMeasureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) override;
  LoomTextMeasurement loomPrepareText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) override;
  LoomTextMeasurement loomPrepareTextForMount(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) override;
  std::pair<float, float> loomGetScrollOffset(SurfaceId, Tag) override;
  std::optional<std::pair<float, float>>
  loomGetViewLocationInSurface(SurfaceId, Tag) override;
  void loomReportUnsupportedInlineAttachment(SurfaceId, Tag);
  TextAttributes loomParseTextAttributes(
      SurfaceId,
      const std::string& componentName,
      const folly::dynamic& props,
      const TextAttributes& inherited,
      ParagraphAttributes& outParagraphAttributes) override;
  void loomUpdateState(const LoomTextStateUpdate&) override;
  void loomUpdateComponentState(
      SurfaceId,
      Tag,
      const std::shared_ptr<const State>&) override;
  void loomDispatchCommand(
      SurfaceId, Tag, const std::string&, const folly::dynamic&) override;
  void loomSendAccessibilityEvent(
      SurfaceId, Tag, const std::string&) override;
  void loomSetIsJSResponder(SurfaceId, Tag, bool, bool) override;
#endif

  void schedulerShouldMergeReactRevision(SurfaceId surfaceId) override;

  void mergeReactRevision(SurfaceId surfaceId);

  void schedulerDidRequestPreliminaryViewAllocation(const ShadowNode &shadowNode) override;

  void schedulerDidDispatchCommand(
      const ShadowView &shadowView,
      const std::string &commandName,
      const folly::dynamic &args) override;

  void schedulerDidSendAccessibilityEvent(const ShadowView &shadowView, const std::string &eventType) override;

  void schedulerDidSetIsJSResponder(const ShadowView &shadowView, bool isJSResponder, bool blockNativeResponder)
      override;

  void schedulerShouldSynchronouslyUpdateViewOnUIThread(Tag tag, const folly::dynamic &props) override;

  void schedulerShouldSynchronouslyUpdateAnimatedProps(const std::unordered_map<Tag, AnimatedProps> &updates) override;

  void schedulerDidUpdateShadowTree(const std::unordered_map<Tag, folly::dynamic> &tagToProps) override;

  void schedulerDidCaptureViewSnapshot(Tag tag, SurfaceId surfaceId) override;

  void schedulerDidSetViewSnapshot(Tag sourceTag, Tag targetTag, SurfaceId surfaceId) override;

  void schedulerDidClearPendingSnapshots() override;

  void recordDirectNativeProps(jint tag, NativeMap* props);

  /**
   * Compact form of recordDirectNativeProps for flat paint payloads (see
   * FabricUIManager.encodeAnimatedPaint): `data` holds [tag, n, payload x n]
   * records; a payload is [keyId, value] pairs, a transform being
   * [kTransform, ops, (opId, value) x ops].
   */
  void recordDirectPaintBatch(jni::alias_ref<jni::JArrayDouble> data, jint length);

  /**
   * Loom animation fast lane: applies the animation props collected since the
   * last flush. Driven by the pre-draw frame callback in FabricUIManager, which
   * runs after the Native Animated module's own callback and before this vsync's
   * mount traversal.
   */
  void flushDirectAnimationFrame();

  void setPixelDensity(float pointScaleFactor);

  void driveCxxAnimations();

  void driveAnimationBackend(jlong frameTimeNanos);

  void drainPreallocateViewsQueue();

  void reportMount(SurfaceId surfaceId);

  void pullAndExecuteTransaction(SurfaceId surfaceId);

  jint findNextFocusableElement(jint parentTag, jint focusedTag, jint direction);

  jintArray getRelativeAncestorList(jint rootTag, jint childTag);

  void uninstallFabricUIManager();

#if LOOM_RENDERER
  void dispatchDirectEvent(
      jint surfaceId,
      jint reactTag,
      jni::alias_ref<jstring> eventName,
      jboolean canCoalesce,
      ReadableNativeMap* params,
      jint eventCategory,
      jlong eventTimestamp,
      jboolean synchronous);
#endif

  // Private member variables
  std::shared_mutex installMutex_;
  std::shared_ptr<FabricMountingManager> mountingManager_;
  std::shared_ptr<Scheduler> scheduler_;
#if LOOM_RENDERER
  std::shared_ptr<LoomEventManager> eventManager_;
  std::shared_ptr<const TextLayoutManager> directTextLayoutManager_;
#endif

  std::shared_ptr<FabricMountingManager> getMountingManager(const char *locationHint);

  // LayoutAnimations
  void onAnimationStarted() override;
  void onAllAnimationsComplete() override;

  std::shared_ptr<LayoutAnimationDriver> animationDriver_;

  // Roots not created through ReactSurface (non-bridgeless) will store their
  // SurfaceHandler here, for other roots we keep a weak reference to the Java
  // owner
  std::unordered_map<SurfaceId, std::variant<SurfaceHandler, jni::weak_ref<SurfaceHandlerBinding::jhybridobject>>>
      surfaceHandlerRegistry_{};
  std::shared_mutex surfaceHandlerRegistryMutex_; // Protects `surfaceHandlerRegistry_`.

  // Track pending transactions, one per surfaceId.
  std::mutex pendingTransactionsMutex_;
  std::vector<MountingTransaction> pendingTransactions_;

  // Pull model: keeps the coordinator that reported a surface's commit alive
  // until the UI thread handles the pull, as iOS does by capturing it in the
  // block it dispatches to the main queue. Java only gets the surface id.
  std::mutex pullableMountingCoordinatorsMutex_;
  std::unordered_map<SurfaceId, std::shared_ptr<const MountingCoordinator>> pullableMountingCoordinators_;

  std::atomic<float> pointScaleFactor_{1};

  bool enableFabricLogs_{false};

  std::shared_ptr<AndroidAnimationChoreographer> animationChoreographer_;
};

} // namespace facebook::react
