/* Loom adapter for react-native-reanimated. See ../README.md. */

#pragma once

#include <react/renderer/uimanager/LoomLayoutAnimations.h>
#include <reanimated/LayoutAnimations/LayoutAnimationsProxyCommon.h>
#include <reanimated/Tools/FeatureFlags.h>

#include <folly/dynamic.h>

#include <atomic>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace reanimated::loom {

using facebook::react::DirectLayoutOperation;
using facebook::react::DirectRemoveOperation;
using facebook::react::LoomLayoutAnimationDriver;
using facebook::react::LoomLayoutAnimationGeometry;
/** React Native's rectangle (Reanimated has its own `Rect`, a size). */
using ReactRect = facebook::react::Rect;
using ReactPoint = facebook::react::Point;
using facebook::react::LoomMountBatch;

/**
 * Reanimated's layout animations on one Loom surface.
 *
 * Reanimated's own proxies keep a copy of every view of the surface (built from
 * the full Fabric mutation stream), because Fabric gives them no tree to look
 * nodes up in when an animation starts. Loom mounts batches instead, and this
 * proxy only follows the views that can animate: views of Animated components,
 * which carry the `nativeID` Reanimated keys entering animations by. Every
 * other view costs one lookup per mount operation that names it.
 *
 * The animations themselves are Reanimated's: this class starts them through
 * the base class (which runs the configured worklet on the UI runtime), and
 * mounts the values the base class collects for each frame.
 *
 * Shared element transitions (with Reanimated's ENABLE_SHARED_ELEMENT_TRANSITIONS
 * flag) follow Reanimated's model too: when the top active
 * `SharedTransitionBoundary` changes, every shared view of the old one with a
 * namesake in the new one is hidden with it, and a copy of it (a "container"
 * at the surface root) animates between their window frames. On iOS a screen
 * that is popped drives the copy with the native transition's progress.
 */
class LoomLayoutAnimationsProxy : public LayoutAnimationsProxyCommon, public LoomLayoutAnimationDriver {
 public:
  LoomLayoutAnimationsProxy(SurfaceId surfaceId, const LayoutAnimationsProxyDependencies &dependencies);
  ~LoomLayoutAnimationsProxy() override;

  // MountingOverrideDelegate: Loom surfaces never mount shadow-tree
  // transactions, so there is nothing to rewrite.
  bool shouldOverridePullTransaction() const override;
  std::optional<MountingTransaction> pullTransaction(
      SurfaceId surfaceId,
      MountingTransaction::Number number,
      const TransactionTelemetry &telemetry,
      ShadowViewMutationList mutations) const override;

  // LayoutAnimationsProxyCommon
  std::optional<SurfaceId> endLayoutAnimation(int tag, bool shouldRemove) override;
  void startSurface(
      const ShadowTree &shadowTree,
      std::weak_ptr<const MountingOverrideDelegate> mountingOverrideDelegate) override;
  void surfaceDidUnmount() override;
  std::optional<SurfaceId> onTransitionProgress(int tag, double progress, bool isClosing, bool isGoingForward)
      override;
  std::optional<SurfaceId> onGestureCancel(int tag) override;

  // LoomLayoutAnimationDriver
  void processBatch(SurfaceId surfaceId, LoomMountBatch &batch, const LoomLayoutAnimationGeometry &geometry)
      override;
  void frame(SurfaceId surfaceId, LoomMountBatch &batch, const LoomLayoutAnimationGeometry &geometry) override;

 private:
  /** What a followed view looks like now, for the ShadowViews of its animations. */
  struct FollowedView {
    std::string componentName;
    folly::dynamic props{folly::dynamic::object()};
    Tag parentTag{0};
    DirectLayoutOperation layout{};
    bool hasLayout{false};
  };

  /**
   * A removed view kept mounted while it, or something under it, exits.
   *
   * React removes a subtree with one batch: a `Remove` for every mounted edge
   * in it (descendants first), the root's `Remove` from its live parent, and a
   * `Delete` per node. A view with an exit animation keeps its whole subtree;
   * an ancestor of one keeps only its exiting branches, and goes when the last
   * of them is done. This is the rule of Reanimated's own proxy
   * (`startAnimationsRecursively`), without its copy of the whole tree.
   */
  struct HeldView {
    Tag parentTag{0};
    /** The parent is a live (React-owned) view, not another held view. */
    bool liveParent{false};
    /** Its own exit animation runs (its subtree stays as it is). */
    bool exiting{false};
    /** Held children of a non-exiting ancestor, in native order. */
    std::vector<Tag> children;
    /**
     * Its subtree's own removals (deepest first) when it exits whole: the
     * subtree stays mounted inside it, and a mounted view cannot be deleted.
     */
    std::vector<DirectRemoveOperation> subtreeRemovals;
    /** Deletes mounted with its removal: its own, and its subtree's if exiting. */
    std::vector<Tag> deletes;
  };

  /**
   * A held view in a live parent. React's indices for that parent do not count
   * it, so later inserts and removals there are moved past it.
   */
  struct HeldInLiveParent {
    Tag tag{0};
    /** How many of React's children of the parent come before it. */
    size_t reactBefore{0};
  };

  /** One `Remove` of the batch being processed. */
  struct RemovedEdge {
    size_t operationIndex;
    Tag parentTag;
    Tag tag;
    size_t index;
  };

  struct BatchRemovals {
    /** Removed edges by parent, in batch order (descending index). */
    std::unordered_map<Tag, std::vector<RemovedEdge>> byParent;
    std::unordered_map<Tag, size_t> deleteOperation;
    std::vector<bool> *skip;
  };

  /** Decides whether `tag` (removed from `parentTag`) stays mounted. */
  bool holdRemoved(Tag tag, Tag parentTag, BatchRemovals &removals);
  /** Holds a removed subtree under an exiting view, deletes included. */
  void holdSubtree(
      Tag tag,
      std::vector<DirectRemoveOperation> &subtreeRemovals,
      std::vector<Tag> &deletes,
      BatchRemovals &removals);
  /** Mounts the removal of a held view whose exit (or last held child) ended. */
  void releaseHeld(Tag tag, LoomMountBatch &out);
  /** Removes every view held in `parentTag` now (the parent is going away). */
  void releaseHeldInParent(Tag parentTag, LoomMountBatch &out);
  /** Native index of React's child `index` of `parentTag` for an insertion. */
  size_t nativeInsertIndex(Tag parentTag, size_t index);
  /** Native index of React's child `index` of `parentTag` for a removal. */
  size_t nativeRemoveIndex(Tag parentTag, size_t index);

  ShadowView shadowViewOf(Tag tag, const FollowedView &view) const;
  /** Mounts the layout (and opacity) an ended animation may have left behind. */
  void reconcileFinalFrame(Tag tag, const LayoutAnimation &animation, LoomMountBatch &out);
  void startEntering(Tag tag, const FollowedView &view, LoomMountBatch &out);
  bool startExiting(Tag tag, const FollowedView &view, Tag parentTag);
  void startLayout(Tag tag, const FollowedView &view, const DirectLayoutOperation &next);

  std::unordered_map<Tag, FollowedView> followed_;
  std::unordered_map<Tag, HeldView> held_;
  std::unordered_map<Tag, std::vector<HeldInLiveParent>> heldInLiveParent_;

  // MARK: Shared element transitions

  static constexpr bool kSharedTransitions = StaticFeatureFlags::getFlag("ENABLE_SHARED_ELEMENT_TRANSITIONS");

  /** A `REASharedTransitionBoundary` or a React Native Screens screen. */
  struct Structural {
    bool boundary{false};
    bool active{false};
    /** A boundary's nearest screen. */
    Tag screen{0};
  };

  /** A copy of a shared view at the surface root that animates between two frames. */
  struct Container {
    Tag source{0};
    Tag target{0};
    /** Driven by a screen transition's progress, not by Reanimated's animation. */
    bool progress{false};
    /** Where it is going. */
    ReactRect targetFrame{};
  };

  /** Where a view was last on screen: its native origin and its layout origin then. */
  struct Seen {
    ReactPoint native;
    std::optional<ReactPoint> layout;
  };

  /** A native screen transition the user drives (iOS back gesture and pop). */
  struct ProgressTransition {
    enum class State { Start, Active, End, Cancelled } state{State::Start};
    Tag sourceScreen{0};
    Tag targetScreen{0};
    double progress{0};
    bool updated{false};
  };

  /** A finished pop whose source screen React has not removed yet. */
  struct PendingPop {
    Tag sourceScreen{0};
    std::vector<Tag> sourceViews;
    bool cancelled{false};
  };

  bool isShared(Tag tag) const;
  /** The nearest boundary above `parentTag` (inclusive), from the renderer. */
  Tag boundaryAbove(Tag parentTag, const LoomLayoutAnimationGeometry &geometry);
  bool refreshBoundaries(const LoomLayoutAnimationGeometry &geometry);
  Tag screenAbove(Tag parentTag, const LoomLayoutAnimationGeometry &geometry) const;
  /** `tag`'s layout frame in the surface, below the native headers of its screens. */
  std::optional<ReactRect> layoutFrame(Tag tag, const LoomLayoutAnimationGeometry &geometry) const;
  /** The last active boundary: the screen on top. */
  Tag findTopBoundary() const;
  /** Shared views of `boundary` by shared tag. */
  std::unordered_map<std::string, Tag> sharedViewsOf(Tag boundary) const;
  void setOpacity(Tag tag, std::optional<double> opacity, LoomMountBatch &out);
  void hide(Tag tag, LoomMountBatch &out);
  void restore(Tag tag, LoomMountBatch &out);
  /** Restores the hidden views of the top boundary that no container is bringing in. */
  void restoreTopScreen(LoomMountBatch &out);
  /** Creates a container that looks like `source` at `frame`; returns its tag. */
  Tag createContainer(Tag source, Tag target, const ReactRect &frame, bool progress, LoomMountBatch &out);
  void removeContainer(Tag tag, LoomMountBatch &out);
  size_t containerIndex(Tag tag) const;
  /** Starts the commit-driven transitions from `before` to `after`. */
  void startSharedTransitions(
      Tag before,
      Tag after,
      const std::unordered_map<Tag, ReactPoint> &beforeOrigins,
      const LoomLayoutAnimationGeometry &geometry,
      LoomMountBatch &out);
  void retargetSharedTransitions(const LoomLayoutAnimationGeometry &geometry);
  void handleProgressTransition(LoomMountBatch &out, const LoomLayoutAnimationGeometry &geometry);

  std::unordered_map<Tag, Structural> structural_;
  /** The nearest boundary of each followed view inside one. */
  std::unordered_map<Tag, Tag> boundaryOf_;
  Tag topBoundary_{0};
  std::unordered_map<Tag, Seen> seen_;
  std::unordered_map<Tag, Container> containers_;
  std::vector<Tag> containerOrder_;
  /** `containerOrder_.size()`, read by the scan in `processBatch` without the lock. */
  std::atomic<size_t> flyingContainers_{0};
  std::unordered_set<Tag> hidden_;
  /** React's views in the surface root (the containers go after them). */
  size_t rootChildren_{0};
  std::optional<ProgressTransition> progress_;
  std::optional<PendingPop> pendingPop_;
};

} // namespace reanimated::loom
