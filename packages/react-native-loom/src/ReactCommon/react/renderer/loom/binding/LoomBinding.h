/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Only the Loom renderer build (LOOM_RENDERER) compiles this header; without it Fabric is untouched.
#if LOOM_RENDERER

#include <react/renderer/loom/worker/LoomWorker.h>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/dynamic.h>
#include <jsi/jsi.h>
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/loom/compat/LoomCompat.h>
#include <react/renderer/loom/host/LoomHost.h>
#include <react/renderer/loom/core/LoomRenderer.h>

namespace facebook::react::loom::internal {
struct Instance;
} // namespace facebook::react::loom::internal

namespace facebook::react {

/**
 * The `nativeFabricUIManager` JavaScript talks to when the direct UI-thread
 * renderer is enabled.
 *
 * There is no shadow tree behind it, so every method of the `FabricUIManager`
 * spec that receives a node is answered from `LoomRenderer`'s own tree.
 * Only the methods that take no node at all - event handler registration,
 * layout animation configuration and the event priority constants - are
 * forwarded to `UIManagerBinding`.
 */
/**
 * A base for `publishSequence_` that no other binding instance shares. Drafts are
 * reachable only through this binding's `NodeRef` host objects, so a collision is
 * not reachable today; this makes it impossible rather than argued.
 */
// An inline variable, not a function-local static: see LoomPlatformTraits.h.
inline std::atomic<uint64_t> gLoomNextPublishSequenceBase{1};
inline uint64_t nextPublishSequenceBase() {
  return gLoomNextPublishSequenceBase.fetch_add(uint64_t{1} << 40);
}

class LoomBinding final : public UIManagerBinding {
 public:
  struct Lifetime;
  /**
   * What a surface is laid out against, plus what text measurement and
   * `measureInWindow` need in order to answer in the same coordinate space the
   * shadow-tree path does.
   */
  struct SurfaceConstraints {
    float minWidth{0};
    float minHeight{0};
    float maxWidth{0};
    float maxHeight{0};
    bool isRTL{false};
    float viewportOffsetX{0};
    float viewportOffsetY{0};
  };

  explicit LoomBinding(std::shared_ptr<UIManager> uiManager);
  ~LoomBinding() override;

  /**
   * Publishes a surface's viewport. Static because the Fabric binding that
   * receives it has no handle on any particular JS binding; constraints are
   * keyed by surface, and the binding that owns the surface picks them up -
   * either immediately, or when it creates that surface's renderer.
   */
  static void setSurfaceConstraints(
      SurfaceId surfaceId,
      float minWidth,
      float minHeight,
      float maxWidth,
      float maxHeight,
      bool isRTL,
      float viewportOffsetX,
      float viewportOffsetY);

  static void setPointScaleFactor(float pointScaleFactor);

  /** Drops a surface's renderer and everything it mounted on the host. */
  static void releaseSurface(SurfaceId surfaceId);
  static void recordNativeProps(Tag tag, folly::dynamic props);
  /**
   * Same, for a payload the platform already knows holds only paint keys
   * (opacity, transform, colours) in their final flat form: no `style`
   * hoisting and no key classification.
   */
  static void recordNativePaintProps(Tag tag, folly::dynamic paint);

  /**
   * One animated view's props from the shared animation backend (Reanimated).
   * `viewApplied` says the platform puts them on the view in the same frame
   * (one batched call for every paint-only view); otherwise the lane writes
   * their paint keys to the view itself, and lays out their layout keys.
   */
  static void recordAnimationProps(Tag tag, folly::dynamic props, bool viewApplied);

  /**
   * Animation fast lane (design §5.2): folds every surface's collected
   * animation frame into the Loom renderer. Installed as the runtime
   * scheduler's rendering-update listener (end of a JS task) and called from the
   * host's before-frame hook (see LoomHost.h), so it must be safe to call at any
   * point between frames and cheap when nothing was written.
   */
  static void flushAnimationFrames();

  /**
   * Fabric compatibility layer: takes a native state update
   * (`State::updateState` -> event dispatcher -> state pipe) addressed to a
   * node of a Loom surface and applies it to that node's ShadowNode in the
   * Loom renderer. False when no Loom surface owns the update's family, so the
   * caller hands it to Fabric's `UIManager::updateState` as usual.
   */
  static bool dispatchComponentState(const StateUpdate& stateUpdate);

  // -------------------------------------------------------------------------
  // Fabric compatibility layer, tree contracts (LoomFabricBridge.cpp).
  //
  // Libraries that address nodes through Fabric (Reanimated, Unistyles,
  // gesture-handler) get real ShadowNodes for the nodes they ask about, built
  // on demand from Loom's renderer. Nothing is built for any other node, and
  // React commits never pay for this.
  // -------------------------------------------------------------------------

  /**
   * The ShadowNode behind a Loom JS node handle (`Bridging::fromJs`). Its
   * family is stable for as long as a library holds it. Null for any object
   * that is not a Loom node handle.
   */
  static std::shared_ptr<const ShadowNode> shadowNodeFromJs(
      jsi::Runtime& runtime,
      const jsi::Object& object);

  /**
   * `UIManager::getNewestCloneOfShadowNode` for a node of a Loom surface: a
   * ShadowNode of its family with its current props and layout. Null when no
   * Loom surface owns the node.
   */
  static std::shared_ptr<const ShadowNode> newestShadowNode(
      const ShadowNode& shadowNode);

  /**
   * `UIManager::getRelativeLayoutMetrics` for a node of a Loom surface, from
   * Loom's layout. Nullopt when no Loom surface owns the node.
   */
  static std::optional<LayoutMetrics> relativeLayoutMetrics(
      const ShadowNode& shadowNode,
      const ShadowNode* ancestorShadowNode,
      LayoutableShadowNode::LayoutInspectingPolicy policy);

  /**
   * `ShadowTree::commit` on a Loom surface that has nodes a library holds: the
   * transaction runs against a tree of those nodes and their ancestors, and
   * the props it changes are applied to Loom's renderer. Nullopt when the
   * surface is not Loom's, holds no such nodes, or the transaction changed no
   * node of them (the caller then commits as usual).
   */
  /**
   * A frame of the surface's layout animations (`ShadowTree::
   * notifyDelegatesOfUpdates`, which Reanimated calls per frame): the
   * surface's layout-animation driver appends the frame and Loom mounts it.
   * False when the surface has no driver (the caller notifies as usual).
   */
  static bool layoutAnimationFrame(SurfaceId surfaceId);

  static std::optional<ShadowTreeCommitStatus> commitOnDemand(
      const ShadowTree& shadowTree,
      const ShadowTreeCommitTransaction& transaction);

  // ---- Support for the library API (react/loom/Loom.h, implemented in LoomApi.cpp) ----------------
  // Members, not statics: a `loom::Runtime` addresses one React instance, whose binding this is.

  /** This instance's library-API state (listeners, the handle `loom::Runtime` holds). Never null. */
  const std::shared_ptr<loom::internal::Instance>& apiInstance() const {
    return apiInstance_;
  }
  /** Calls `visit` for the id of every running surface of this binding. */
  void apiForEachSurface(const std::function<void(SurfaceId)>& visit);
  /** The surface of this binding with a mounted node `tag`. Never blocks. */
  std::optional<SurfaceId> apiSurfaceOf(Tag tag);
  /**
   * Runs `read` with `surfaceId`'s renderer and constraints. `synced`: after the commits already
   * submitted were applied (`LoomWorker::readSynced`); otherwise as of the latest computed commit
   * without waiting (`LoomWorker::readLatest`). False when `read` was not run.
   */
  bool apiRead(
      SurfaceId surfaceId,
      bool synced,
      const std::function<void(const LoomRenderer&, const SurfaceConstraints&)>& read);
  /** A JS-lane props write of a node of the surface (any thread). */
  void apiUpdateProps(SurfaceId surfaceId, Tag tag, folly::dynamic props);
  /** A native-lane (`viewApplied` false) or animation-lane write of a node of the surface. */
  void apiRecordNativeProps(SurfaceId surfaceId, Tag tag, folly::dynamic props, bool knownPaint, bool viewApplied);
  /** A native state update for a node of the surface. */
  bool apiUpdateState(SurfaceId surfaceId, const StateUpdate& update) {
    return submitComponentState(surfaceId, update);
  }

  static void shutdownWorkerForUIManager(UIManager& manager);
  static void synchronizeWorkerForUIManager(UIManager& manager);

  jsi::Value get(jsi::Runtime& runtime, const jsi::PropNameID& name) override;

  /** Dispatches NativeDOMCxx calls against direct node references. */
  jsi::Value getDirectDOM(
      jsi::Runtime& runtime,
      const std::string& method,
      const jsi::Value* arguments,
      size_t count);

 private:
  struct NodeRef;

  /**
   * The reconciler below is private to this class and depends only on the
   * renderer and the draft model, which is exactly what makes it testable
   * without a JSI runtime or a UIManager. The native suite drives it through
   * this named accessor rather than duplicating the draft model.
   */
  friend struct LoomBindingTestAccess;

  struct SurfaceToken {
    SurfaceId surfaceId{0};
    uint64_t generation{0};
    std::atomic<bool> active{true};
  };

  struct PropsPatch {
    std::shared_ptr<const PropsPatch> previous;
    folly::dynamic props;
    std::optional<std::string> rawText;
    // Set once when the patch is created so that publication and the
    // text-safety decision never re-scan a props payload.
    bool unsafeText{false};
    // Written by the computation owner. A later commit reads the boundary on
    // the UI thread while an earlier job may still be computing, so the marker
    // is atomic.
    mutable std::atomic<bool> applied{false};
  };

  struct RetainedLiveChild {
    Tag tag{0};
    uint64_t identity{0};
  };

  /** React-facing speculative data only; it owns no Yoga or mounted state. */
  struct DraftNode {
    SurfaceId surfaceId{0};
    uint64_t surfaceGeneration{0};
    uint64_t identity{0};
    Tag tag{0};
    std::shared_ptr<const std::string> componentName;
    /** Resolved from `componentName` once, when the draft is created. */
    LoomComponentTable::Roles roles;
    InstanceHandle::Shared instanceHandle;
    std::shared_ptr<const folly::dynamic> initialProps;
    std::shared_ptr<const std::string> initialRawText;
    bool hasInitialProps{false};
    std::shared_ptr<const PropsPatch> propsTail;
    /**
     * Components with their own descriptor only: that descriptor, and the props
     * it parsed from the JSI values of this version (as Fabric parses them on
     * the JS thread). Kept across compaction: the next props diff is parsed on
     * top of them.
     */
    const ComponentDescriptor* compatDescriptor{nullptr};
    std::shared_ptr<const Props> compatProps;
    std::shared_ptr<std::vector<std::shared_ptr<DraftNode>>> children{
        std::make_shared<std::vector<std::shared_ptr<DraftNode>>>()};
    std::shared_ptr<const std::vector<RetainedLiveChild>> retainedLiveChildren;
    bool childrenExplicit{true};
    bool committedSnapshot{false};
    /**
     * A hidden clone (`cloneNodeHidden`): this version is the source version
     * hidden by an offscreen boundary. Committing the source version again
     * shows the node with exactly its own props, as Fabric does.
     */
    bool offscreenHidden{false};
    // Sticky record of text effects in this node's already-committed props. The
    // applied patch chain is dropped once it has been reconciled, so this keeps
    // the text-safety decision stable without retaining the whole history.
    bool retainedUnsafeTextProps{false};
    /**
     * Cached result of the text-effects scan over `initialProps`. That scan is a
     * full recursive walk of the props, and the worker path never drops
     * `initialProps` (it does not compact), so without this it repeats for every
     * unchanged node on every commit. A published draft is immutable by contract,
     * and `initialProps` is only ever written when the host builds a version, so
     * the answer cannot change while this version is live.
     */
    bool initialPropsTextScanned{false};
    bool initialPropsHasTextEffects{false};
    /**
     * Subtree-wide answers `freeze` accumulates: whether the subtree contains a
     * text component, whether anything in it depends on host text state, and
     * whether it retains a live snapshot. Cached per draft version so a version
     * that was already published is reused instead of walked again - a change
     * builds a new version object, so the answers cannot change while this one is
     * live.
     */
    struct FreezeFacts {
      bool hasText{false};
      bool unsafeTextProps{false};
      bool retainsLiveSnapshot{false};
    };
    FreezeFacts freezeFacts;
    /**
     * Set once the commit that published this version has been *applied*, so the
     * live renderer is known to hold this subtree. This is the worker path's
     * equivalent of `committedSnapshot`; it must not be set at publish time,
     * because a published commit can still be in flight with the live tree
     * reflecting an older revision.
     */
    bool workerCommitted{false};
    bool published{
        false}; // UI staging only; mutate a new version after publication.
    /**
     * The last publication that froze this draft. A draft is reachable from more
     * than one place during a commit (several roots, shared versions), and this
     * replaces the visited set that deduplicated them: one comparison instead of
     * a hash insert per node per commit.
     */
    uint64_t frozenCommit{0};
  };

  /** The only thing JavaScript ever sees of a rendered node. */
  struct NodeRef : jsi::HostObject {
    NodeRef(
        SurfaceId surfaceId,
        Tag tag,
        std::shared_ptr<SurfaceToken> surfaceToken,
        std::shared_ptr<DraftNode> draft)
        : surfaceId(surfaceId),
          tag(tag),
          surfaceToken(std::move(surfaceToken)),
          draft(std::move(draft)) {}
    SurfaceId surfaceId;
    Tag tag;
    std::shared_ptr<SurfaceToken> surfaceToken;
    std::shared_ptr<DraftNode> draft;
  };

  /** The child list React accumulates over a commit, handed to `completeRoot`.
   */
  struct ChildSet : jsi::HostObject {
#if LOOM_PROFILE
    std::chrono::steady_clock::time_point createdAt{
        std::chrono::steady_clock::now()};
    int64_t createdAtEpochNanoseconds{0};
    uint64_t createCallNanoseconds{0};
#endif
    SurfaceId surfaceId{0};
    std::shared_ptr<SurfaceToken> surfaceToken;
    std::vector<std::shared_ptr<NodeRef>> nodes;
  };

  struct PendingRootCommit {
    std::shared_ptr<SurfaceToken> surfaceToken;
    std::vector<std::shared_ptr<NodeRef>> children;
    uint64_t profileGeneration{0};
  };

#if LOOM_PROFILE
  enum class ProfiledHostMethod : size_t {
    CreateNode,
    CloneNode,
    CloneNodeWithNewProps,
    CloneNodeWithNewChildren,
    CloneNodeWithNewChildrenAndProps,
    CreateChildSet,
    AppendChildToSet,
    AppendChild,
    CompleteRoot,
    Count,
  };

  struct HostMethodStats {
    uint64_t calls{0};
    uint64_t totalNanoseconds{0};
  };

  struct HostProfile {
    std::array<HostMethodStats, static_cast<size_t>(ProfiledHostMethod::Count)>
        methods{};
    int64_t firstHostCallEpochNanoseconds{0};
    int64_t firstHostCallSteadyNanoseconds{0};
  };

  struct HostCallScope {
    HostCallScope(
        LoomBinding& binding,
        SurfaceId surfaceId,
        ProfiledHostMethod method);
    ~HostCallScope();
    LoomBinding& binding;
    SurfaceId surfaceId;
    uint64_t generation;
    ProfiledHostMethod method;
    std::chrono::steady_clock::time_point start;
  };
#endif

  struct WorkerCommit {
    SurfaceId surfaceId;
    std::shared_ptr<SurfaceToken> token;
    /** The instance's library-API state: commit listeners run from the worker's reconcile. */
    std::shared_ptr<loom::internal::Instance> api;
    std::vector<std::shared_ptr<DraftNode>> roots;
    InstanceHandle::Shared documentHandle;
    /** Made by requestAnimationFrame work (AnimationFrameWork). */
    bool animationFrame{false};
    /** Versions this publish newly froze: sizes the commit's selection. */
    size_t publishedDrafts{0};
  };
  void publishWorkerCommit(
      SurfaceId,
      const std::vector<std::shared_ptr<NodeRef>>&,
      bool);
  static void reconcileWorkerCommit(
      LoomRenderer&,
      const WorkerCommit&);
  /**
   * Reconciles one surface's mounted bookkeeping against the identity set a
   * commit produced, as a delta: entries that already agree are left in place
   * rather than erased and re-inserted. `identities` is the complete
   * post-reconciliation live set, so absence still means "removed" even once the
   * freeze walk is allowed to skip unchanged subtrees.
   */
  void applyPublishedIdentities(
      const std::shared_ptr<SurfaceToken>& token,
      const TagMap<uint64_t>& identities,
      const std::vector<std::shared_ptr<DraftNode>>& committedDrafts,
      const std::vector<Tag>* removedTags = nullptr);
  void submitWorkerJob(
      std::shared_ptr<SurfaceToken>,
      std::unique_ptr<LoomWorker::Job>,
      /** Set when the job ran on the UI thread instead of being queued. */
      bool* ranSynchronously = nullptr);
  void submitWorkerOperation(
      std::shared_ptr<SurfaceToken>,
      LoomWorker::Task,
      std::function<void()> = {},
      bool synchronous = false,
      bool profileCommit = false,
      bool inlineEligible = false);
  std::unique_ptr<LoomWorker> worker_;
  /** Fabric compatibility layer; created with the first renderer. */
  std::shared_ptr<LoomCompat> compat_;
  bool submitComponentState(SurfaceId surfaceId, const StateUpdate& update);
  /**
   * Loom: smoothed compute time of recent small commits (few changed
   * nodes), in ms. While it stays under `kInlineBudgetMs` a small commit is
   * computed on the submitting thread when the worker is idle, where the
   * hand-off (two wakeups, a JNI thread attach per job) costs more than the
   * work. Written by whichever thread computed the commit.
   */
  std::atomic<float> smallCommitMs_{0.0f};
  static constexpr double kInlineBudgetMs = 3.0;
  static constexpr double kInlineMaxUnits = 64.0;
  /** Longest the pre-traversal drain waits for the worker (main mode only). */
  static constexpr std::chrono::microseconds kFrameDrainBudget{8000};
  /**
   * Whether text can be measured on the worker. Java answers on the UI thread
   * only, so off-UI JS reads the last answer the UI thread produced (unknown
   * counts as "no", which merely computes that commit on the UI thread).
   */
  std::atomic<int> canMeasureTextOnWorker_{-1};
  /**
   * The platform host registered in the UI manager's context container
   * (`LoomHost::registerIn`), resolved once when the binding is created. Never
   * null; its text services may be.
   */
  LoomHost* host() const {
    return host_;
  }
  LoomTextServices* text() const {
    return text_;
  }
  std::shared_ptr<LoomHost> hostOwner_;
  LoomHost* host_{nullptr};
  LoomTextServices* text_{nullptr};
  bool canMeasureTextOnWorker();
  void refreshCanMeasureTextOnWorker();
  /**
   * In-place updates: a node whose children did not change but whose props (or
   * raw text) did keeps its React instance, so React clones none of its
   * ancestors. React reports the change in its commit phase (`loomPatchNode`)
   * and ends the commit with `loomFlushPatches`, which hands every recorded
   * patch to the worker as one job. JS-thread state.
   */
  struct PendingPatch {
    Tag tag{0};
    uint64_t identity{0};
    folly::dynamic props;
    std::optional<std::string> rawText;
  };
  std::unordered_map<SurfaceId, std::vector<PendingPatch>> pendingPatches_;
  void flushPendingPatches(SurfaceId surfaceId);
  /**
   * The platform's UI-thread executor, resolved on first use and cached. Null
   * until the UI manager's delegate is installed.
   */
  std::function<void(std::function<void()>)> platformUIExecutor();
  std::mutex uiExecutorMutex_;
  std::function<void(std::function<void()>)> uiExecutor_;
  /**
   * Runs `read` on the UI thread and returns its answer. On the UI thread it is
   * a direct call; elsewhere the caller waits for the UI thread (JS to UI only),
   * and a stalled UI thread yields `fallback` after a logged timeout. With
   * `LoomPlatformTraits::liveReadsOnAnyThread` it is a direct call everywhere.
   */
  std::optional<std::pair<float, float>> readOnUIThread(
      std::function<std::optional<std::pair<float, float>>(LoomHost&)> read,
      std::optional<std::pair<float, float>> fallback);
  bool forceNextCommitSync_{false};
  Tag replayingNativePropsTag_{0};
  const folly::dynamic* replayingNativeProps_{nullptr};
  std::shared_ptr<const folly::dynamic> stageNativeProps(
      Tag,
      uint64_t,
      std::shared_ptr<SurfaceToken>,
      folly::dynamic);
  void setWorkerNativeProps(const NodeRef&, folly::dynamic);
  /**
   * Published-but-not-yet-mounted identities, nested per surface. Every access
   * already knows the surface (the identity check itself is against
   * `{surfaceId, identity}`), and nothing looks a tag up without one, so the
   * surface can be the key and the value shrinks to the identity alone.
   */
  std::unordered_map<SurfaceId, TagMap<uint64_t>>
      submittedIdentities_;
  /** createNode: component name -> one shared string (JS thread only). */
  std::unordered_map<std::string, std::shared_ptr<const std::string>>
      internedComponentNames_;
  /**
   * createNode: interned component name -> its compat descriptor (null for a
   * component that has none). Looked up once per name (JS thread only).
   */
  std::unordered_map<const std::string*, const ComponentDescriptor*>
      compatDescriptorByName_;
  /** The compat descriptor for an interned component name (JS thread). */
  const ComponentDescriptor* compatDescriptorFor(
      const std::shared_ptr<const std::string>& componentName);
  /** Parses `value` with the draft's compat descriptor on top of its props. */
  void parseCompatProps(jsi::Runtime& rt, DraftNode& draft, const jsi::Value& value);
  TagMap<uint64_t> mountedIdentities_;
  TagMap<std::shared_ptr<SurfaceToken>> mountedTokens_;
  /**
   * The last native props written for one tag, replayed onto the view after a
   * commit. `identity` and `props` must be initialized here: a fresh entry is
   * value-initialized by `nativePropsOverlays_[surfaceId][tag]`, and writers
   * only clear `props` when the identity changes, so an uninitialized `props`
   * (a null dynamic) would be indexed as an object on the very first write for
   * a tag whose published identity is zero, throwing
   * "TypeError: expected dynamic type 'object', but had type 'null'".
   */
  struct NativePropsOverlay {
    SurfaceId surfaceId{0};
    uint64_t identity{0};
    uint64_t sequence{0};
    folly::dynamic props{folly::dynamic::object()};
  };
  uint64_t nativePropsSequence_{0};
  /**
   * Identifies one publication. `freeze` stamps every draft it visits with it so
   * a shared version is only processed once, without a visited set.
   */
  // Started from a process-wide counter so a stamp can never be reissued with
  // the same value by a later binding, which would make `freeze` mistake a draft
  // stamped by an earlier binding for one it had already visited this commit.
  uint64_t publishSequence_{nextPublishSequenceBase()};
  /**
   * Staged immediate-update overlays, nested per surface. They are read per
   * surface - the replay after every applied batch, and teardown - so a flat map
   * made both scans O(all overlays on all surfaces).
   */
  std::unordered_map<SurfaceId, TagMap<NativePropsOverlay>>
      nativePropsOverlays_;
  /**
   * The tags each surface has mounted: the per-surface index of `mountedTokens_`
   * (which stays keyed by tag, for the platform's tag-only lookups). Commits and
   * surface release walk only their own surface through it, never every mounted
   * tag of every surface. Maintained only by `setMountedLocked` and
   * `eraseMountedLocked`.
   */
  std::unordered_map<SurfaceId, TagMap<bool>> mountedTagsBySurface_;
  /** Records `tag` as mounted with `identity` by `token`'s surface. `stateMutex_` held. */
  void setMountedLocked(Tag tag, uint64_t identity, const std::shared_ptr<SurfaceToken>& token);
  /**
   * Forgets a mounted `tag` and everything kept for it (its native-props overlay).
   * `stateMutex_` held.
   */
  void eraseMountedLocked(Tag tag);

  // ---------------------------------------------------------------------
  // Animation fast lane.
  //
  // Animated prop writes no longer become one worker job per call. They are
  // collected into a per-frame buffer keyed by tag, paint keys reach the native
  // view immediately, and one flush per frame either borrows the renderer when
  // the worker is idle or merges the frame into the single animation job a
  // surface may have queued.
  // ---------------------------------------------------------------------

  /**
   * One tag's collected animated props for the frame being gathered, split by
   * the class the key table assigned (design §5.1). `identity` is the draft
   * identity at write time, so a write that outlives its node is dropped
   * instead of landing on whoever holds the tag now.
   */
  struct AnimatedWrite {
    uint64_t identity{0};
    folly::dynamic paint{folly::dynamic::object()};
    folly::dynamic layout{folly::dynamic::object()};
    /** Paint keys no native view has received yet (see writeAnimatedProps). */
    folly::dynamic view{folly::dynamic::object()};
    bool text{false};
  };

  /** The frame being collected, nested per surface: every access knows it. */
  std::unordered_map<SurfaceId, TagMap<AnimatedWrite>>
      animationFrameBuffer_;

  /**
   * The one animation job that may be queued per surface. Until the worker
   * claims this frame a flush merges the newer writes into it instead of
   * appending another job, which is what keeps the queue at one entry however
   * many nodes and frames are animating (design §5.4). The claim is the mutex,
   * so it is race-free with the compute that is about to run.
   */
  struct AnimationFrame {
    std::mutex mutex;
    bool claimed{false};
    TagMap<folly::dynamic> paint;
    TagMap<folly::dynamic> layout;
    TagMap<folly::dynamic> view;
    std::vector<Tag> tags;

    /** Merges newer patches; false once this frame has been claimed. */
    bool merge(
        std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
        std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches,
        std::vector<std::pair<Tag, folly::dynamic>>& viewPatches);
    /**
     * Takes the merged frame exactly once, on the thread that will apply it.
     */
    bool take(
        std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
        std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches,
        std::vector<std::pair<Tag, folly::dynamic>>& viewPatches);
  };

  std::unordered_map<SurfaceId, std::shared_ptr<AnimationFrame>>
      pendingAnimationFrames_;

  /**
   * Per-surface cost of the synchronous lane and its adaptive state
   * (design §5.7). The lane runs on the UI thread only while its measured cost
   * stays inside the budget; past it, layout frames take the coalesced async
   * job, which costs at most a frame of lag and never blocks the UI thread.
   */
  struct AnimationLaneStats {
    double emaMicros{0};
    int overBudgetFrames{0};
    int asyncProbeCounter{0};
    bool asyncMode{false};
  };

  std::unordered_map<SurfaceId, AnimationLaneStats> animationLaneStats_;

  /**
   * The lane's entry point: classifies one animated write into the frame being
   * collected, keeps its paint keys as the replayable overlay, and (when the
   * platform has not applied them already) puts them on the native view before
   * returning (design §5.1). Called on the UI thread.
   */
  void writeAnimatedProps(
      SurfaceId surfaceId,
      Tag tag,
      uint64_t identity,
      ClassifiedAnimatedProps classified,
      bool applyPaintToView,
      bool viewAlreadyApplied = false);

  static void recordNativePropsImpl(
      Tag tag,
      folly::dynamic props,
      bool knownPaint,
      bool viewApplied = true);
  /**
   * One binding's part of `recordNativePropsImpl`: true when this binding owns a mounted node `tag`
   * (of `surfaceId`, when given) and took the write.
   */
  bool recordNativePropsHere(
      std::optional<SurfaceId> surfaceId,
      Tag tag,
      folly::dynamic& props,
      bool knownPaint,
      bool viewApplied);

  /** Flushes one surface's collected frame (design §5.2). */
  void flushAnimationFrame(SurfaceId surfaceId);
  /** Flushes every surface's collected frame of this binding. */
  void flushOwnAnimationFrames();
  /**
   * Makes sure collected animation writes reach the screen even when no platform frame hook flushes
   * them (iOS has none; Android's choreographer flush makes this a no-op there): the first write
   * after a flush posts one flush to the UI thread. `stateMutex_` may be held.
   */
  void scheduleAnimationFlush();
  std::atomic<bool> animationFlushPosted_{false};

  /** Queues or runs the frame as the surface's single animation job. */
  void deferAnimationFrame(
      SurfaceId surfaceId,
      std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
      std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches,
      std::vector<std::pair<Tag, folly::dynamic>>& viewPatches,
      bool needsAsync);

  /** Applies paint keys to the native view through the synchronous delegate. */
  void applyAnimatedPaintToView(Tag tag, const folly::dynamic& props);

  /** Drops the paint overlays of tags whose frame is folded. */
  void erasePaintOverlays(SurfaceId surfaceId, const std::vector<Tag>& tags);

  /** Records an observed synchronous cost and updates the adaptive mode. */
  void recordAnimationLaneCost(SurfaceId surfaceId, double micros);

  /** True while this surface's lane may still run synchronously. */
  bool animationLaneAllowsSync(SurfaceId surfaceId);

  /** Emits one `LoomAnimLane` profile line when profiling is enabled. */
  void traceAnimationLane(
      SurfaceId surfaceId,
      bool synchronous,
      double micros,
      size_t nodes);
  /**
   * Fabric families handed to libraries, per surface and tag. Held for as long
   * as the node is mounted, as a shadow tree holds its nodes' families:
   * libraries key by the family and treat one nobody else holds as unmounted
   * (Unistyles drops those). Released when the renderer removes the node.
   */
  struct FabricFamily {
    ShadowNodeFamily::Shared family;
    uint64_t identity{0};
    /** Props parsed for the last node built from the renderer. */
    Props::Shared props;
    uint64_t propsGeneration{0};
    const ComponentDescriptor* propsDescriptor{nullptr};
  };
  std::mutex fabricFamiliesMutex_;
  std::unordered_map<SurfaceId, TagMap<FabricFamily>> fabricFamilies_;
  /** Set once any family was handed out: removals only pay for this after. */
  std::atomic<bool> hasFabricFamilies_{false};
  /**
   * Families of removed nodes, destroyed on the JS thread: a family holds the
   * node's instance handle, a JS reference that must not die on the UI thread.
   */
  std::vector<ShadowNodeFamily::Shared> releasedFabricFamilies_;
  /** Drops the families of nodes a commit removed. Under `stateMutex_`. */
  void releaseFabricFamilies(
      SurfaceId surfaceId,
      const std::vector<Tag>* removedTags,
      const TagMap<uint64_t>& identities);
  /**
   * Props of the node handed for a JS node handle, per component: parsed once,
   * since such a node only has to carry its identity and kind.
   */
  std::unordered_map<const ComponentDescriptor*, Props::Shared> handleProps_;
  /**
   * Runs `body` with the binding and live renderer that own `surfaceId`; the
   * binding cannot be torn down meanwhile. False when no binding owns it.
   */
  static bool withSurfaceRenderer(
      SurfaceId surfaceId,
      const std::function<
          void(LoomBinding&, const std::shared_ptr<LoomRenderer>&)>& body);
  /** The ShadowNode handed out for a JS node handle. */
  std::shared_ptr<const ShadowNode> handleShadowNode(const NodeRef& ref);
  /** `fabricFamilies_` entry for `tag`, (re)created for `identity`. Locked. */
  ShadowNodeFamily::Shared fabricFamilyLocked(
      const ComponentDescriptor& descriptor,
      SurfaceId surfaceId,
      Tag tag,
      uint64_t identity,
      const InstanceHandle::Shared& instanceHandle);
  /**
   * A ShadowNode of `tag` built from the renderer (props, layout relative to
   * its parent, `children`). The worker must be synchronized. Null when the
   * renderer does not know `tag`.
   */
  std::shared_ptr<ShadowNode> fabricNodeFromRenderer(
      LoomRenderer& liveRenderer,
      Tag tag,
      std::shared_ptr<const std::vector<std::shared_ptr<const ShadowNode>>>
          children);
  std::optional<ShadowTreeCommitStatus> commitOnDemandImpl(
      LoomRenderer& liveRenderer,
      const ShadowTree& shadowTree,
      const ShadowTreeCommitTransaction& transaction);
  /** Waits for every submitted commit to be applied (any thread). */
  void synchronizeForFabricRead();

  LoomRenderer& renderer(SurfaceId surfaceId);
  /** Reuses the source ref's live token instead of taking `stateMutex_` for it. */
  std::shared_ptr<NodeRef> makeNodeRef(
      const NodeRef& source,
      std::shared_ptr<DraftNode> draft);
  std::shared_ptr<NodeRef> makeNodeRef(
      SurfaceId surfaceId,
      Tag tag,
      std::shared_ptr<DraftNode> draft = nullptr);
  std::shared_ptr<DraftNode> cloneDraft(
      const std::shared_ptr<NodeRef>& source,
      bool replaceChildren = false);
  /** True when a props payload carries text effects anywhere inside it. */
  static bool propsContainTextEffects(const folly::dynamic& value);
  /**
   * Links a newly created patch to the oldest patch that is still pending and
   * folds the text effects of the applied prefix it drops into the draft. The
   * live renderer node already holds every applied patch's result, so keeping
   * that prefix would only grow the retained chain with app uptime.
   */
  static void linkPendingPatches(DraftNode& draft, PropsPatch& patch);

  /**
   * The draft graph a commit selected, resolved against the live renderer.
   * Shared by both reconcilers: the worker one runs it on the worker thread,
   * the synchronous one borrows the same computation owner on the UI thread.
   */
  struct CommitSelection {
    struct Entry {
      std::shared_ptr<DraftNode> draft;
      std::vector<Tag> childTags;
      bool childrenChanged{false};
    };
    std::vector<Entry> selected;
    std::vector<Tag> rootChildren;
    Tag rootTag{0};
    /** Selected tag -> live identity, for removed-edge capture. */
    TagMap<uint64_t> selectedIdentities;
    /**
     * Live subtrees that can lose nodes in this commit: the old children a
     * changed parent no longer lists, and the old version of a node whose
     * identity changed. Removed-edge capture walks only these.
     */
    std::vector<Tag> removalRoots;
    /** Nonzero when the selected graph contains an unsupported inline
     * attachment; the caller decides how to fail. */
    Tag unsupportedAttachmentTag{0};
  };

  /**
   * Walks `rootDrafts` and the live tree, validates the draft graph and returns
   * what the commit has to reconcile. Throws on a stale, cyclic, multiply-parented
   * or identity-conflicting draft.
   */
  static CommitSelection selectCommitDrafts(
      LoomRenderer& liveRenderer,
      SurfaceId surfaceId,
      const SurfaceToken& surfaceToken,
      const std::vector<std::shared_ptr<DraftNode>>& rootDrafts,
      size_t expectedDrafts = 0);

  /**
   * Applies a selection to the live renderer: captures the cleanup edges the
   * removed parents own, creates or re-identifies each selected node, replays its
   * pending props patches, commits the new child order, marks those patches
   * applied and completes the root.
   *
   * `compact` additionally drops the applied history and the creation-time draft
   * fields. Only the synchronous path may do that: the worker path leaves the
   * drafts to the host, which still owns them while the result is in flight.
   */
  static void reconcileSelectedDrafts(
      LoomRenderer& liveRenderer,
      SurfaceId surfaceId,
      const CommitSelection& selection,
      bool compact,
      loom::internal::Instance& api);
  bool isRawTextRef(const NodeRef& ref) const;
  std::shared_ptr<SurfaceToken> tokenFor(SurfaceId surfaceId);
  SurfaceConstraints constraintsFor(SurfaceId surfaceId) const;
  /** The renderer that owns `tag`; tags are unique across surfaces. */
  LoomRenderer* rendererOwning(Tag tag);

  void applyConstraints(
      SurfaceId surfaceId,
      const SurfaceConstraints& constraints);
  /** Propagates the host point scale factor to every renderer. UI thread only. */
  void applyPointScaleFactor(float pointScaleFactor);

  /**
   * Runs `effect` on the UI thread after every commit the worker already owns,
   * without blocking the calling thread.
   *
   * Host calls such as commands, accessibility events and the JS responder flag
   * need to observe the renderer only after the in-flight computation has been
   * applied, because the checks that guard them read the renderer's node
   * registry, which the worker mutates while it computes. Publication
   * bookkeeping runs on the UI thread in the same order the results are applied,
   * so ordering the work there is equivalent to blocking in `synchronize()` -
   * and a surface that stops, or a job whose surface went stale, simply never
   * runs it, which is what the blocking version did as well.
   */
  void submitOrderedUIEffect(SurfaceId surfaceId, std::function<void()> effect);
  void releaseSurfaceInternal(SurfaceId surfaceId);
  void publishRootCommit(
      SurfaceId surfaceId,
      const std::vector<std::shared_ptr<NodeRef>>& children,
      const std::shared_ptr<SurfaceToken>& surfaceToken,
      uint64_t profileGeneration);
  void drainPendingRootCommits(SurfaceId surfaceId);

  std::shared_ptr<NodeRef> nodeRef(
      jsi::Runtime& runtime,
      const jsi::Value& value) const;
  bool isLiveNodeRef(const NodeRef& ref) const;
  static std::vector<Tag> tagsOf(const std::shared_ptr<ChildSet>& set);

  /** `measure()` semantics: own size, origin relative to the parent. */
  bool parentMetrics(Tag tag, LoomRenderer::Metrics& out);
  /** `measureInWindow()` semantics: own size, origin relative to the surface.
   */
  bool windowMetrics(
      Tag tag,
      LoomRenderer::Metrics& out,
      bool includeTransform = true);

  /**
   * Guards the bookkeeping below when JavaScript runs off the UI thread, where
   * host calls (JS) and platform callbacks (UI) both reach it: `renderers_`,
   * `surfaceTokens_`, `surfaceGenerations_`, `submittedIdentities_`,
   * `mountedIdentities_`, `mountedTokens_`, `nativePropsOverlays_`,
   * `animationFrameBuffer_`, `pendingAnimationFrames_` and
   * `animationLaneStats_`. Recursive because the UI thread re-enters through
   * synchronous view updates. Hold it only around map accesses: never across a
   * worker barrier, a JNI call, or a view effect.
   */
  mutable std::recursive_mutex stateMutex_;
  /** `renderers_` lookup that is safe against a concurrent surface teardown. */
  std::shared_ptr<LoomRenderer> findRenderer(SurfaceId surfaceId) const;
  std::unordered_map<SurfaceId, std::shared_ptr<LoomRenderer>> renderers_;
  std::unordered_map<SurfaceId, std::shared_ptr<SurfaceToken>> surfaceTokens_;
  // One counter for the binding, not one per surface: identities stay unique across a surface
  // restart and nothing per surface is left behind when it is released.
  uint64_t nextDraftIdentity_{0};
  std::unordered_map<SurfaceId, uint64_t> surfaceGenerations_;
#if LOOM_PROFILE
  std::unordered_map<SurfaceId, uint64_t> profileCommitGeneration_;
  std::unordered_map<SurfaceId, std::unordered_map<uint64_t, HostProfile>>
      profileHostCalls_;
#endif
  std::unordered_map<SurfaceId, std::deque<PendingRootCommit>>
      pendingRootCommits_;
  std::unordered_set<SurfaceId> drainingRootCommits_;
  // A surface may stop and restart synchronously from a Java mount/event
  // callback. Serialize publications by surface even when restart creates a
  // new renderer while the old renderer is still unwinding.
  std::unordered_set<SurfaceId> publishingRootCommits_;
  std::unordered_map<SurfaceId, InstanceHandle::Shared>
      pendingDocumentInstanceHandles_;
  std::unordered_map<SurfaceId, InstanceHandle::Shared>
      documentInstanceHandles_;
  std::shared_ptr<Lifetime> lifetime_;
  std::shared_ptr<loom::internal::Instance> apiInstance_;
};

/**
 * Lets static callbacks and library handles reach a binding safely: they hold the gate shared while
 * they use `binding`; the destructor takes it exclusively and nulls `binding`.
 */
struct LoomBinding::Lifetime {
  std::shared_mutex gate;
  LoomBinding* binding{nullptr};
};

} // namespace facebook::react

#endif // LOOM_RENDERER
