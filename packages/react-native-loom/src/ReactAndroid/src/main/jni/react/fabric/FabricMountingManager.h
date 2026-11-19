/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>
#include <unordered_map>
#include <optional>
#include <unordered_set>
#include <vector>

#include <fbjni/fbjni.h>
#include <react/fabric/JFabricUIManager.h>
#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/mapbuffer/MapBuffer.h>
#include <react/renderer/mounting/ShadowView.h>
#include <react/renderer/loom/host/LoomLiveReads.h>
#include <react/renderer/loom/mounting/LoomMountBatch.h>
#include <react/renderer/loom/text/LoomTextMeasurement.h>
#include <react/renderer/uimanager/primitives.h>

namespace facebook::react {

/** Logs the Java stack trace of a JNI exception raised by a Loom renderer operation. */
void logDirectRendererJavaException(const char* operation, const jni::JniException& error);

class MountingTransaction;
struct ShadowView;

class FabricMountingManager final {
 public:
  FabricMountingManager(jni::global_ref<JFabricUIManager::javaobject> &javaUIManager);
  FabricMountingManager(const FabricMountingManager &) = delete;
  ~FabricMountingManager();

  void onSurfaceStart(SurfaceId surfaceId);

  void onSurfaceStop(SurfaceId surfaceId);

  /*
   * Can be called from any thread. Returns whether a view was queued for
   * preallocation (it is skipped on the main thread).
   */
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

  /*
   * Converts the transaction's mutations into an IntBufferBatchMountItem and
   * hands it to Java.
   *
   * In the push model (`synchronous` = false), the batch is
   * scheduled onto the UI thread asynchronously.
   *
   * In the pull model (`synchronous` = true) the batch is
   * applied immediately on the calling (UI) thread.
   */
  void executeMount(const MountingTransaction &transaction, bool synchronous = false);

  /*
   * Pull model: notify Java that a transaction is available for `surfaceId` so
   * the UI thread can pull it via a PullTransactionMountItem.
   */
  void onTransactionAvailable(SurfaceId surfaceId);

#if LOOM_RENDERER
  /*
   * Applies one flat, ordered batch produced by `LoomRenderer`. The batch
   * is a mixed array of operation codes and operands; non-numeric operands
   * (component names, prop maps) are pushed into the Java object array, the
   * rest into the int instruction array.
   */
  bool directCanMeasureTextOnWorker();
  void directApplyBatch(SurfaceId surfaceId, LoomMountBatch operations);
  void directFailSurface(SurfaceId surfaceId, const std::string& reason);

  /*
   * Publishes the resolved text payload of a paragraph. Android renders text
   * from this state and from props alone, so the attributed string has to be
   * handed to the host this way.
   */
  void directUpdateState(const LoomTextStateUpdate& update);
  /** Fabric compatibility layer: a compat node's own state object. */
  void directUpdateComponentState(
      SurfaceId surfaceId,
      Tag tag,
      const std::shared_ptr<const State>& state);
  std::pair<float, float> directGetScrollOffset(SurfaceId surfaceId, Tag tag);
  /**
   * The scroll positions of `scrollTags` and, when `locationTag` is not zero,
   * that view's location, in one call to Java. Without a location it is safe
   * on any thread; the location needs the UI thread.
   */
  LoomLiveReads directGetLiveReads(
      SurfaceId surfaceId,
      const std::vector<Tag>& scrollTags,
      Tag locationTag);
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

  // React Native's own signatures (taking the ShadowView), kept exported: libraries link against
  // them. They forward to the (surfaceId, tag) overloads above.
  void dispatchCommand(const ShadowView &shadowView, const std::string &commandName, const folly::dynamic &args);
  void sendAccessibilityEvent(const ShadowView &shadowView, const std::string &eventType);
  void setIsJSResponder(const ShadowView &shadowView, bool isJSResponder, bool blockNativeResponder);

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

#if LOOM_RENDERER
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
