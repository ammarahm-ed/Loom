/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <react/renderer/uimanager/UIThreadRendererWorker.h>
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
#include <react/renderer/uimanager/UIThreadRenderer.h>

namespace facebook::react {

/**
 * The `nativeFabricUIManager` JavaScript talks to when the direct UI-thread
 * renderer is enabled.
 *
 * There is no shadow tree behind it, so every method of the `FabricUIManager`
 * spec that receives a node is answered from `UIThreadRenderer`'s own tree.
 * Only the methods that take no node at all - event handler registration,
 * layout animation configuration and the event priority constants - are
 * forwarded to `UIManagerBinding`.
 */
/**
 * A base for `publishSequence_` that no other binding instance shares. Drafts are
 * reachable only through this binding's `NodeRef` host objects, so a collision is
 * not reachable today; this makes it impossible rather than argued.
 */
inline uint64_t nextPublishSequenceBase() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(uint64_t{1} << 40);
}

class UIThreadRendererBinding final : public UIManagerBinding {
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

  explicit UIThreadRendererBinding(std::shared_ptr<UIManager> uiManager);
  ~UIThreadRendererBinding() override;

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
  friend struct UIThreadRendererBindingTestAccess;

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
    InstanceHandle::Shared instanceHandle;
    std::shared_ptr<const folly::dynamic> initialProps;
    std::shared_ptr<const std::string> initialRawText;
    bool hasInitialProps{false};
    std::shared_ptr<const PropsPatch> propsTail;
    std::shared_ptr<std::vector<std::shared_ptr<DraftNode>>> children{
        std::make_shared<std::vector<std::shared_ptr<DraftNode>>>()};
    std::shared_ptr<const std::vector<RetainedLiveChild>> retainedLiveChildren;
    bool childrenExplicit{true};
    bool committedSnapshot{false};
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
#if RN_PROFILE_UI_THREAD_RENDERER
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

#if RN_PROFILE_UI_THREAD_RENDERER
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
        UIThreadRendererBinding& binding,
        SurfaceId surfaceId,
        ProfiledHostMethod method);
    ~HostCallScope();
    UIThreadRendererBinding& binding;
    SurfaceId surfaceId;
    uint64_t generation;
    ProfiledHostMethod method;
    std::chrono::steady_clock::time_point start;
  };
#endif

#if UI_THREAD_RENDERER_WORKER
  struct WorkerCommit {
    SurfaceId surfaceId;
    std::shared_ptr<SurfaceToken> token;
    std::vector<std::shared_ptr<DraftNode>> roots;
    InstanceHandle::Shared documentHandle;
  };
  void publishWorkerCommit(
      SurfaceId,
      const std::vector<std::shared_ptr<NodeRef>>&,
      bool);
  static void reconcileWorkerCommit(
      UIThreadRenderer&,
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
      const std::unordered_map<Tag, uint64_t>& identities,
      const std::vector<std::shared_ptr<DraftNode>>& committedDrafts);
  void submitWorkerJob(
      std::shared_ptr<SurfaceToken>,
      std::unique_ptr<UIThreadRendererWorker::Job>);
  void submitWorkerOperation(
      std::shared_ptr<SurfaceToken>,
      UIThreadRendererWorker::Task,
      std::function<void()> = {},
      bool synchronous = false,
      bool profileCommit = false);
  std::unique_ptr<UIThreadRendererWorker> worker_;
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
  std::unordered_map<SurfaceId, std::unordered_map<Tag, uint64_t>>
      submittedIdentities_;
  std::unordered_map<Tag, uint64_t> mountedIdentities_;
  std::unordered_map<Tag, std::shared_ptr<SurfaceToken>> mountedTokens_;
  struct NativePropsOverlay {
    SurfaceId surfaceId;
    uint64_t identity;
    uint64_t sequence;
    folly::dynamic props;
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
  std::unordered_map<SurfaceId, std::unordered_map<Tag, NativePropsOverlay>>
      nativePropsOverlays_;
#endif
  UIThreadRenderer& renderer(SurfaceId surfaceId);
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
    std::unordered_map<Tag, uint64_t> selectedIdentities;
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
      UIThreadRenderer& liveRenderer,
      SurfaceId surfaceId,
      const SurfaceToken& surfaceToken,
      const std::vector<std::shared_ptr<DraftNode>>& rootDrafts);

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
      UIThreadRenderer& liveRenderer,
      SurfaceId surfaceId,
      const CommitSelection& selection,
      bool compact);
  bool isRawTextRef(const NodeRef& ref) const;
  std::shared_ptr<SurfaceToken> tokenFor(SurfaceId surfaceId);
  SurfaceConstraints constraintsFor(SurfaceId surfaceId) const;
  /** The renderer that owns `tag`; tags are unique across surfaces. */
  UIThreadRenderer* rendererOwning(Tag tag);

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
  bool parentMetrics(Tag tag, UIThreadRenderer::Metrics& out);
  /** `measureInWindow()` semantics: own size, origin relative to the surface.
   */
  bool windowMetrics(
      Tag tag,
      UIThreadRenderer::Metrics& out,
      bool includeTransform = true);

  std::unordered_map<SurfaceId, std::shared_ptr<UIThreadRenderer>> renderers_;
  std::unordered_map<SurfaceId, std::shared_ptr<SurfaceToken>> surfaceTokens_;
  std::unordered_map<SurfaceId, uint64_t> nextDraftIdentity_;
  std::unordered_map<SurfaceId, uint64_t> surfaceGenerations_;
#if RN_PROFILE_UI_THREAD_RENDERER
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
};

} // namespace facebook::react
