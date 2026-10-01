/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

#include <fbjni/fbjni.h>
#include <react/fabric/JFabricUIManager.h>
#include <react/renderer/uimanager/UIThreadDirectMountBatch.h>
#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/mapbuffer/MapBuffer.h>
#include <react/renderer/uimanager/primitives.h>
#include <react/renderer/uimanager/UIThreadTextMeasurement.h>

namespace facebook::react {

class MountingTransaction;
struct ShadowView;

class FabricMountingManager final {
 public:
  FabricMountingManager(jni::global_ref<JFabricUIManager::javaobject> &javaUIManager);
  FabricMountingManager(const FabricMountingManager &) = delete;
  ~FabricMountingManager();

  void onSurfaceStart(SurfaceId surfaceId);

  void onSurfaceStop(SurfaceId surfaceId);

  bool maybePreallocateShadowNode(const ShadowNode &shadowNode);

  void destroyUnmountedShadowNode(const ShadowNodeFamily &family);

  /*
   * Drains preallocatedViewsQueue_ by calling preallocateShadowView on each
   * item in the queue. Can be called by any thread.
   */
  void drainPreallocateViewsQueue();

  /*
   * Preallocates a view on the Java side and registers the tag in
   * allocatedViewRegistry_ so that executeMount skips the redundant Create
   * mount item for this tag.
   */
  void preallocateShadowView(const ShadowView &shadowView);

  /*
   * Returns true if the given tag is registered in allocatedViewRegistry_
   * for the given surface. A registered tag means executeMount will skip
   * the Create mount item (the view was already preallocated).
   */
  bool isViewAllocated(SurfaceId surfaceId, Tag tag);

  void executeMount(const MountingTransaction &transaction);

#if UI_THREAD_RENDERER
  /*
   * Applies one flat, ordered batch produced by `UIThreadRenderer`. The batch
   * is a mixed array of operation codes and operands; non-numeric operands
   * (component names, prop maps) are pushed into the Java object array, the
   * rest into the int instruction array.
   */
  bool directCanMeasureTextOnWorker();
  void directApplyBatch(SurfaceId surfaceId, UIThreadDirectMountBatch operations);
  void directFailSurface(SurfaceId surfaceId, const std::string& reason);

  /*
   * Publishes the resolved text payload of a paragraph. Android renders text
   * from this state and from props alone, so the attributed string has to be
   * handed to the host this way.
   */
  void directUpdateState(
      SurfaceId surfaceId,
      Tag tag,
      const std::string& componentName,
      const folly::dynamic& props,
      const AttributedString& attributedString,
      const ParagraphAttributes& paragraphAttributes,
      const UIThreadTextMeasurement& prepared);
  std::pair<float, float> directGetScrollOffset(SurfaceId surfaceId, Tag tag);
  std::optional<std::pair<float, float>> directGetViewLocationInSurface(
      SurfaceId surfaceId,
      Tag tag);
  void directDispatchCommand(
      SurfaceId surfaceId,
      Tag tag,
      const std::string& commandName,
      const folly::dynamic& args);
  void directSendAccessibilityEvent(
      SurfaceId surfaceId,
      Tag tag,
      const std::string& eventType);
  void directSetIsJSResponder(
      SurfaceId surfaceId,
      Tag tag,
      bool isJSResponder,
      bool blockNativeResponder);
#endif

  void dispatchCommand(SurfaceId surfaceId, Tag tag, const std::string &commandName, const folly::dynamic &args);

  void sendAccessibilityEvent(SurfaceId surfaceId, Tag tag, const std::string &eventType);

  void setIsJSResponder(SurfaceId surfaceId, Tag tag, bool isJSResponder, bool blockNativeResponder);

  void onAnimationStarted();

  void onAllAnimationsComplete();

  void synchronouslyUpdateViewOnUIThread(Tag viewTag, const folly::dynamic &props);

  void captureViewSnapshot(Tag tag, SurfaceId surfaceId);

  void setViewSnapshot(Tag sourceTag, Tag targetTag, SurfaceId surfaceId);

  void clearPendingSnapshots();

  void scheduleReactRevisionMerge(SurfaceId surfaceId);

 private:
  bool isOnMainThread();

  jni::global_ref<JFabricUIManager::javaobject> javaUIManager_;

  std::recursive_mutex commitMutex_;

#if UI_THREAD_RENDERER
  struct PendingDirectState {
    Tag tag;
    std::optional<MapBuffer> mapBuffer;
    jni::global_ref<jobject> stateWrapper;
    jni::global_ref<jobject> preparedTextLayout;
  };
  std::unordered_map<SurfaceId, std::vector<PendingDirectState>>
      pendingDirectStates_;
#endif

  /*
   * Protects preallocatedViewsQueue_.
   */
  std::mutex preallocateMutex_;

  /*
   * A queue of views to be preallocated on the Java side.
   */
  std::vector<ShadowView> preallocatedViewsQueue_{};

  std::unordered_map<SurfaceId, std::unordered_set<Tag>> allocatedViewRegistry_{};
  std::recursive_mutex allocatedViewsMutex_;
};

} // namespace facebook::react
