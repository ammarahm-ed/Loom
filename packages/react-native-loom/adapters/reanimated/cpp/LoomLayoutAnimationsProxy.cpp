/* Loom adapter for react-native-reanimated. See ../README.md. */

#include "LoomLayoutAnimationsProxy.h"
#include "LoomReanimatedAdapter.h"

#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include <reanimated/LayoutAnimations/LayoutAnimationsUtils.h>

#include <algorithm>
#include <unistd.h>
#include <chrono>
#ifdef ANDROID
#include <android/log.h>
#include <folly/json.h>
#endif
#include <functional>
#include <unordered_set>
#include <utility>
#include <variant>

namespace reanimated::loom {

using facebook::react::DirectCreateOperation;
using facebook::react::DirectDeleteOperation;
using facebook::react::DirectInsertOperation;
using facebook::react::DirectMountOperation;
using facebook::react::DirectRemoveOperation;
using facebook::react::DirectUpdatePropsOperation;

namespace {

/** The `nativeID` Reanimated gives an Animated component, if `props` has one. */
const folly::dynamic *nativeIdOf(const folly::dynamic &props) {
  if (!props.isObject()) {
    return nullptr;
  }
  const auto it = props.find("nativeID");
  return it != props.items().end() && it->second.isString() ? &it->second : nullptr;
}

Tag tagOf(const DirectMountOperation &operation) {
  return std::visit([](const auto &typed) -> Tag { return typed.tag; }, operation);
}

constexpr const char *kBoundaryComponent = "REASharedTransitionBoundary";

bool isScreenComponent(const std::string &name) {
  return name == "RNSScreen" || name == "RNSModalScreen";
}

bool isStructuralComponent(const std::string &name) {
  return name == kBoundaryComponent || isScreenComponent(name);
}

bool boundaryIsActive(const folly::dynamic &props, bool fallback) {
  if (!props.isObject()) {
    return fallback;
  }
  const auto it = props.find("isActive");
  if (it == props.items().end()) {
    return fallback;
  }
  return it->second.isBool() ? it->second.getBool() : fallback;
}

/** The raw props a platform that mounts from raw props needs (Android). */
folly::dynamic rawPropsOf([[maybe_unused]] const Props::Shared &props) {
#ifdef RN_SERIALIZABLE_STATE
  return props ? props->rawProps : folly::dynamic::object();
#else
  return folly::dynamic::object();
#endif
}

} // namespace

std::shared_ptr<LayoutAnimationsProxyRegistry> createLayoutAnimationsProxyRegistry(
    const LayoutAnimationsProxyDependencies &dependencies) {
  auto registry = std::make_shared<LayoutAnimationsProxyRegistry>(
      [dependencies](const SurfaceId surfaceId) -> std::shared_ptr<LayoutAnimationsProxyCommon> {
        return std::make_shared<LoomLayoutAnimationsProxy>(surfaceId, dependencies);
      });
  // Reanimated registers a surface from its commit hook, on the surface's first
  // shadow-tree commit. A Loom surface does not commit there, so its proxy is
  // registered when the surface starts (and now, for surfaces already running).
  std::weak_ptr<LayoutAnimationsProxyRegistry> weakRegistry = registry;
  dependencies.uiManager->addOnSurfaceStartCallback([weakRegistry](const ShadowTree &shadowTree) {
    if (auto strongRegistry = weakRegistry.lock()) {
      strongRegistry->registerSurface(shadowTree);
    }
  });
  dependencies.uiManager->getShadowTreeRegistry().enumerate(
      [&registry](const ShadowTree &shadowTree, bool & /*stop*/) { registry->registerSurface(shadowTree); });
  return registry;
}

LoomLayoutAnimationsProxy::LoomLayoutAnimationsProxy(
    const SurfaceId surfaceId,
    const LayoutAnimationsProxyDependencies &dependencies)
    : LayoutAnimationsProxyCommon(surfaceId, dependencies) {}

LoomLayoutAnimationsProxy::~LoomLayoutAnimationsProxy() {
  facebook::react::loomUnregisterLayoutAnimationDriver(surfaceId_, this);
}

bool LoomLayoutAnimationsProxy::shouldOverridePullTransaction() const {
  return false;
}

std::optional<MountingTransaction> LoomLayoutAnimationsProxy::pullTransaction(
    const SurfaceId surfaceId,
    const MountingTransaction::Number number,
    const TransactionTelemetry &telemetry,
    ShadowViewMutationList mutations) const {
  return MountingTransaction{surfaceId, number, std::move(mutations), telemetry};
}

void LoomLayoutAnimationsProxy::startSurface(
    const ShadowTree &shadowTree,
    std::weak_ptr<const MountingOverrideDelegate> /*mountingOverrideDelegate*/) {
  // Not installed on the mounting coordinator: Loom hands this proxy its
  // batches through the driver registry instead.
  const auto revision = shadowTree.getCurrentRevision();
  if (revision.rootShadowNode) {
    const auto lock = std::unique_lock<std::recursive_mutex>(mutex);
    const auto &size = revision.rootShadowNode->getLayoutMetrics().frame.size;
    window_ = {size.width, size.height};
  }
  facebook::react::loomRegisterLayoutAnimationDriver(
      surfaceId_, std::dynamic_pointer_cast<LoomLayoutAnimationDriver>(shared_from_this()));
}

void LoomLayoutAnimationsProxy::surfaceDidUnmount() {
  facebook::react::loomUnregisterLayoutAnimationDriver(surfaceId_, this);
  LayoutAnimationsProxyCommon::surfaceDidUnmount();
  if constexpr (kSharedTransitions) {
    const auto lock = std::unique_lock<std::recursive_mutex>(mutex);
    structural_.clear();
    boundaryOf_.clear();
    topBoundary_ = 0;
    seen_.clear();
    containers_.clear();
    containerOrder_.clear();
    flyingContainers_.store(0, std::memory_order_relaxed);
    hidden_.clear();
    progress_.reset();
    pendingPop_.reset();
  }
}

ShadowView LoomLayoutAnimationsProxy::shadowViewOf(const Tag tag, const FollowedView &view) const {
  const auto &descriptor = componentDescriptorRegistry_->at(view.componentName);
  ShadowView shadowView;
  shadowView.componentName = descriptor.getComponentName();
  shadowView.componentHandle = descriptor.getComponentHandle();
  shadowView.surfaceId = surfaceId_;
  shadowView.tag = tag;
  shadowView.traits = descriptor.getTraits();
  const PropsParserContext context{surfaceId_, *contextContainer_};
  shadowView.props = descriptor.cloneProps(context, nullptr, RawProps(folly::dynamic(view.props)));
  if (view.hasLayout) {
    shadowView.layoutMetrics.frame.origin = {view.layout.x, view.layout.y};
    shadowView.layoutMetrics.frame.size = {view.layout.width, view.layout.height};
  }
  return shadowView;
}

void LoomLayoutAnimationsProxy::startEntering(const Tag tag, const FollowedView &view, LoomMountBatch &out) {
  const auto config = layoutAnimationsManager_->getLayoutAnimationConfig(tag, LayoutAnimationType::ENTERING);
  if (!config) {
    return;
  }
  const auto shadowView = shadowViewOf(tag, view);
  if (!isViewKind(shadowView)) {
    return;
  }
  const auto opacity = getViewProps(shadowView).opacity;
  enqueueLayoutAnimation(ManagedLayoutAnimationStart{
      .tag = tag,
      .type = LayoutAnimationType::ENTERING,
      .before = shadowView,
      .after = shadowView,
      .parentTag = view.parentTag,
      .opacity = opacity,
      .config = config,
  });
  // Hidden until the animation's first frame, as Reanimated mounts an
  // entering view; the first frame restores the opacity it does not animate.
  const PropsParserContext context{surfaceId_, *contextContainer_};
  folly::dynamic hidden = folly::dynamic::object("opacity", 0);
  auto hiddenProps = componentDescriptorRegistry_->at(shadowView.componentHandle)
                         .cloneProps(context, shadowView.props, RawProps(folly::dynamic(hidden)));
  out.append(DirectUpdatePropsOperation{.tag = tag, .props = std::move(hidden), .parsedProps = std::move(hiddenProps)});
}

bool LoomLayoutAnimationsProxy::startExiting(const Tag tag, const FollowedView &view, const Tag parentTag) {
  if (!layoutAnimationsManager_->shouldAnimateExiting(tag, true)) {
    return false;
  }
  const auto config = layoutAnimationsManager_->takeExitingAnimationConfigAndClearTag(tag);
  if (!config) {
    return false;
  }
  const auto shadowView = shadowViewOf(tag, view);
  enqueueLayoutAnimation(ManagedLayoutAnimationStart{
      .tag = tag,
      .type = LayoutAnimationType::EXITING,
      .before = shadowView,
      .after = shadowView,
      .parentTag = parentTag,
      .config = config,
  });
  return true;
}

void LoomLayoutAnimationsProxy::startLayout(
    const Tag tag,
    const FollowedView &view,
    const DirectLayoutOperation &next) {
  const auto config = layoutAnimationsManager_->getLayoutAnimationConfig(tag, LayoutAnimationType::LAYOUT);
  if (!config) {
    return;
  }
  const auto before = shadowViewOf(tag, view);
  auto nextView = view;
  nextView.layout = next;
  nextView.hasLayout = true;
  const auto after = shadowViewOf(tag, nextView);
  enqueueLayoutAnimation(ManagedLayoutAnimationStart{
      .tag = tag,
      .type = LayoutAnimationType::LAYOUT,
      .before = before,
      .after = after,
      .parentTag = view.parentTag,
      .config = config,
  });
}

size_t LoomLayoutAnimationsProxy::nativeRemoveIndex(const Tag parentTag, const size_t index) {
  auto &held = heldInLiveParent_.at(parentTag);
  size_t native = index;
  for (auto &entry : held) {
    if (entry.reactBefore <= index) {
      ++native;
    } else {
      --entry.reactBefore;
    }
  }
  return native;
}

size_t LoomLayoutAnimationsProxy::nativeInsertIndex(const Tag parentTag, const size_t index) {
  auto &held = heldInLiveParent_.at(parentTag);
  size_t native = index;
  for (auto &entry : held) {
    if (entry.reactBefore < index) {
      ++native;
    } else {
      ++entry.reactBefore;
    }
  }
  return native;
}

void LoomLayoutAnimationsProxy::holdSubtree(
    const Tag tag,
    std::vector<DirectRemoveOperation> &subtreeRemovals,
    std::vector<Tag> &deletes,
    BatchRemovals &removals) {
  const auto edges = removals.byParent.find(tag);
  if (edges == removals.byParent.end()) {
    return;
  }
  for (const auto &edge : edges->second) {
    (*removals.skip)[edge.operationIndex] = true;
    holdSubtree(edge.tag, subtreeRemovals, deletes, removals);
    subtreeRemovals.push_back(DirectRemoveOperation{.parentTag = tag, .tag = edge.tag, .index = edge.index});
    if (const auto deleted = removals.deleteOperation.find(edge.tag); deleted != removals.deleteOperation.end()) {
      (*removals.skip)[deleted->second] = true;
      deletes.push_back(edge.tag);
    }
  }
}

bool LoomLayoutAnimationsProxy::holdRemoved(const Tag tag, const Tag parentTag, BatchRemovals &removals) {
  bool exiting = false;
  if (const auto followedIt = followed_.find(tag); followedIt != followed_.end()) {
    exiting = startExiting(tag, followedIt->second, parentTag);
  }
  std::vector<Tag> deletes;
  std::vector<DirectRemoveOperation> subtreeRemovals;
  std::vector<std::pair<size_t, Tag>> kept;
  if (exiting) {
    holdSubtree(tag, subtreeRemovals, deletes, removals);
  } else if (const auto edges = removals.byParent.find(tag); edges != removals.byParent.end()) {
    // Removed children that stay are skipped; the others go now. Removals run
    // in descending index order, so skipping one leaves the lower indices of
    // the ones that do run valid.
    for (const auto &edge : edges->second) {
      if (holdRemoved(edge.tag, tag, removals)) {
        (*removals.skip)[edge.operationIndex] = true;
        kept.emplace_back(edge.index, edge.tag);
      }
    }
  }
  if (!exiting && kept.empty()) {
    return false;
  }
  if (const auto deleted = removals.deleteOperation.find(tag); deleted != removals.deleteOperation.end()) {
    (*removals.skip)[deleted->second] = true;
    deletes.push_back(tag);
  }
  std::sort(kept.begin(), kept.end());
  auto &held = held_[tag];
  held.parentTag = parentTag;
  held.liveParent = false;
  held.exiting = exiting;
  held.subtreeRemovals = std::move(subtreeRemovals);
  held.deletes = std::move(deletes);
  held.children.clear();
  for (const auto &[index, child] : kept) {
    held.children.push_back(child);
  }
  return true;
}

void LoomLayoutAnimationsProxy::releaseHeld(const Tag tag, LoomMountBatch &out) {
  const auto it = held_.find(tag);
  if (it == held_.end()) {
    return;
  }
  const auto held = std::move(it->second);
  held_.erase(it);

  size_t index = 0;
  if (held.liveParent) {
    auto &siblings = heldInLiveParent_[held.parentTag];
    for (size_t position = 0; position < siblings.size(); ++position) {
      if (siblings[position].tag == tag) {
        // Held siblings are kept in native order, so the ones before this one
        // are exactly the held views that precede it.
        index = siblings[position].reactBefore + position;
        siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(position));
        break;
      }
    }
    if (siblings.empty()) {
      heldInLiveParent_.erase(held.parentTag);
    }
  } else if (const auto parentIt = held_.find(held.parentTag); parentIt != held_.end()) {
    auto &children = parentIt->second.children;
    const auto position = std::find(children.begin(), children.end(), tag);
    index = static_cast<size_t>(position - children.begin());
    if (position != children.end()) {
      children.erase(position);
    }
  }

  // As React removes a subtree: its inner edges (deepest first), then its root,
  // then the deletes.
  for (const auto &removal : held.subtreeRemovals) {
    out.append(removal);
  }
  out.append(DirectRemoveOperation{.parentTag = held.parentTag, .tag = tag, .index = index});
  for (const auto deleted : held.deletes) {
    out.append(DirectDeleteOperation{.tag = deleted});
    followed_.erase(deleted);
  }
  followed_.erase(tag);

  // A held ancestor goes with its last exiting branch.
  if (!held.liveParent) {
    if (const auto parentIt = held_.find(held.parentTag);
        parentIt != held_.end() && !parentIt->second.exiting && parentIt->second.children.empty()) {
      releaseHeld(held.parentTag, out);
    }
  }
}

void LoomLayoutAnimationsProxy::releaseHeldInParent(const Tag parentTag, LoomMountBatch &out) {
  const auto siblingsIt = heldInLiveParent_.find(parentTag);
  if (siblingsIt == heldInLiveParent_.end()) {
    return;
  }
  // Leaves first, so every removal mounts against a parent that is still there.
  const std::function<void(Tag)> releaseTree = [&](const Tag tag) {
    const auto it = held_.find(tag);
    if (it == held_.end()) {
      return;
    }
    if (it->second.exiting) {
      cancelLayoutAnimation(tag);
    }
    const auto children = it->second.children;
    for (const auto child : children) {
      releaseTree(child);
    }
    releaseHeld(tag, out);
  };
  std::vector<Tag> roots;
  for (const auto &entry : siblingsIt->second) {
    roots.push_back(entry.tag);
  }
  for (const auto root : roots) {
    releaseTree(root);
  }
}

void LoomLayoutAnimationsProxy::processBatch(
    const SurfaceId /*surfaceId*/,
    LoomMountBatch &batch,
    const LoomLayoutAnimationGeometry &geometry) {
  // Cheap scan first: a batch that creates no Animated component, names no
  // followed view and touches no parent that holds an exiting view is left as
  // it is. With shared transitions, one that touches a boundary or a screen
  // (`structure`) may change the screen on top, and one that changes the
  // surface root's children moves the containers' indices.
  bool relevant = false;
  bool structure = false;
  // While a copy flies, any batch can move its target (a pushed screen is laid
  // out again once it is mounted, and the screen's own frame is native).
  const bool flying = kSharedTransitions && flyingContainers_.load(std::memory_order_relaxed) > 0;
  relevant = flying;
  for (const auto &operation : batch.operations) {
    if (relevant && (!kSharedTransitions || structure)) {
      break;
    }
    if (const auto *create = std::get_if<DirectCreateOperation>(&operation)) {
      if (nativeIdOf(create->props) != nullptr) {
        relevant = true;
      }
      if constexpr (kSharedTransitions) {
        if (isStructuralComponent(create->componentName)) {
          relevant = structure = true;
        }
      }
      continue;
    }
    if constexpr (kSharedTransitions) {
      if (!structural_.empty() && structural_.contains(tagOf(operation))) {
        relevant = structure = true;
        continue;
      }
      if (const auto *insert = std::get_if<DirectInsertOperation>(&operation); insert && insert->parentTag == surfaceId_) {
        relevant = true;
      } else if (const auto *remove = std::get_if<DirectRemoveOperation>(&operation);
                 remove && remove->parentTag == surfaceId_) {
        relevant = true;
      }
    }
    if (!followed_.empty() && followed_.contains(tagOf(operation))) {
      relevant = true;
      continue;
    }
    if (!heldInLiveParent_.empty()) {
      Tag parentTag = 0;
      if (const auto *insert = std::get_if<DirectInsertOperation>(&operation)) {
        parentTag = insert->parentTag;
      } else if (const auto *remove = std::get_if<DirectRemoveOperation>(&operation)) {
        parentTag = remove->parentTag;
      } else if (std::holds_alternative<DirectDeleteOperation>(operation)) {
        parentTag = tagOf(operation);
      }
      if (heldInLiveParent_.contains(parentTag)) {
        relevant = true;
      }
    }
  }
  if (!relevant) {
    return;
  }

  // The shared views on screen now, measured on their native views before
  // this batch mounts: one of them may fly to the next screen. The UI thread
  // answers, so this runs without the proxy's lock (`frame` takes it there).
  std::unordered_map<Tag, ReactPoint> beforeOrigins;
  if constexpr (kSharedTransitions) {
    if (structure) {
      std::vector<Tag> onScreen;
      {
        const auto lock = std::unique_lock<std::recursive_mutex>(mutex);
        if (topBoundary_ != 0 && !progress_ && !pendingPop_) {
          for (const auto &[name, tag] : sharedViewsOf(topBoundary_)) {
            onScreen.push_back(tag);
          }
        }
      }
      for (const auto tag : onScreen) {
        if (const auto origin = geometry.nativeOriginInSurface(tag)) {
          beforeOrigins.emplace(tag, *origin);
        }
      }
    }
  }

  auto lock = std::unique_lock<std::recursive_mutex>(mutex);
  auto configLock = layoutAnimationsManager_->lockAndFlushConfigUpdates();

  const auto count = batch.operations.size();
  std::vector<bool> skip(count, false);
  std::vector<Tag> entering;
  std::vector<std::pair<size_t, DirectLayoutOperation>> layoutChanges;
  BatchRemovals removals;
  removals.skip = &skip;
  std::vector<RemovedEdge> edges;

  LoomMountBatch released;
  LoomMountBatch appended;

  // Boundaries and screens first, so the views inserted below find theirs.
  if constexpr (kSharedTransitions) {
    if (structure) {
      for (const auto &operation : batch.operations) {
        if (const auto *create = std::get_if<DirectCreateOperation>(&operation);
            create && isStructuralComponent(create->componentName)) {
          auto &entry = structural_[create->tag];
          entry = Structural{};
          entry.boundary = create->componentName == kBoundaryComponent;
          entry.active = entry.boundary && boundaryIsActive(create->props, false);
        }
      }
    }
  }

  for (size_t index = 0; index < count; ++index) {
    auto &operation = batch.operations[index];
    if constexpr (kSharedTransitions) {
      if (structure) {
        const auto tag = tagOf(operation);
        if (const auto structuralIt = structural_.find(tag); structuralIt != structural_.end()) {
          auto &entry = structuralIt->second;
          if (const auto *update = std::get_if<DirectUpdatePropsOperation>(&operation)) {
            if (entry.boundary) {
              entry.active = boundaryIsActive(update->props, entry.active);
            }
          } else if (const auto *insert = std::get_if<DirectInsertOperation>(&operation)) {
            if (entry.boundary) {
              entry.screen = screenAbove(insert->parentTag, geometry);
            }
          }
        }
      }
    }
    if (auto *create = std::get_if<DirectCreateOperation>(&operation)) {
      if (const auto *nativeId = nativeIdOf(create->props)) {
        transferConfigFromNativeID(nativeId->getString(), create->tag);
        auto &view = followed_[create->tag];
        view = FollowedView{};
        view.componentName = create->componentName;
        view.props = create->props;
      }
      continue;
    }
    if (const auto *remove = std::get_if<DirectRemoveOperation>(&operation)) {
      const RemovedEdge edge{index, remove->parentTag, remove->tag, remove->index};
      removals.byParent[remove->parentTag].push_back(edge);
      edges.push_back(edge);
      continue;
    }
    if (std::holds_alternative<DirectDeleteOperation>(operation)) {
      removals.deleteOperation.emplace(tagOf(operation), index);
      continue;
    }
    const auto tag = tagOf(operation);
    const auto followedIt = followed_.find(tag);
    if (followedIt == followed_.end()) {
      continue;
    }
    auto &view = followedIt->second;
    if (auto *update = std::get_if<DirectUpdatePropsOperation>(&operation)) {
      if (update->props.isObject()) {
        for (const auto &[key, value] : update->props.items()) {
          if (value.isNull()) {
            view.props.erase(key);
          } else {
            view.props[key] = value;
          }
        }
      }
      // A view a shared transition hides stays hidden through React's updates.
      if (kSharedTransitions && !hidden_.empty() && hidden_.contains(tag)) {
        folly::dynamic hiddenOpacity = folly::dynamic::object("opacity", 0);
        if (update->props.isObject()) {
          update->props["opacity"] = 0;
        }
        if (update->parsedProps) {
          const PropsParserContext context{surfaceId_, *contextContainer_};
          update->parsedProps = componentDescriptorRegistry_->at(view.componentName)
                                    .cloneProps(context, update->parsedProps, RawProps(std::move(hiddenOpacity)));
        }
      }
    } else if (const auto *insert = std::get_if<DirectInsertOperation>(&operation)) {
      view.parentTag = insert->parentTag;
      entering.push_back(tag);
      // Every Animated view: Reanimated registers a shared tag once the view is
      // mounted, so whether it is shared is not known yet.
      if constexpr (kSharedTransitions) {
        // From its parent in React's tree: the host parent it is inserted into
        // skips flattened views, the boundary among them.
        if (const auto boundary = boundaryAbove(geometry.parentOf(tag), geometry); boundary != 0) {
          boundaryOf_[tag] = boundary;
          // Inside a boundary that is not active, a shared view waits hidden
          // until a transition brings it in (or its screen comes on top).
          if (!structural_.at(boundary).active && boundary != topBoundary_ && isShared(tag)) {
            hide(tag, appended);
          }
        } else {
          boundaryOf_.erase(tag);
        }
      }
    } else if (const auto *layout = std::get_if<DirectLayoutOperation>(&operation)) {
      if (view.hasLayout) {
        layoutChanges.emplace_back(index, *layout);
      } else {
        view.layout = *layout;
        view.hasLayout = true;
      }
    }
  }

  // A live parent that goes away takes the views it holds with it, first.
  for (const auto &[tag, _] : removals.deleteOperation) {
    if (heldInLiveParent_.contains(tag)) {
      releaseHeldInParent(tag, released);
    }
  }

  // Exits: a removed subtree's root is removed from a parent that stays.
  std::unordered_set<Tag> heldRoots;
  if (!followed_.empty()) {
    for (const auto &edge : edges) {
      // A removal whose view is not deleted is a move to another parent.
      if (removals.deleteOperation.contains(edge.parentTag) || !removals.deleteOperation.contains(edge.tag) ||
          held_.contains(edge.tag)) {
        continue;
      }
      if (holdRemoved(edge.tag, edge.parentTag, removals)) {
        skip[edge.operationIndex] = true;
        auto &held = held_.at(edge.tag);
        held.liveParent = true;
        heldRoots.insert(edge.tag);
      }
    }
  }

  // Layout transitions: the frame moves with the animation, not at once.
  for (const auto &[index, layout] : layoutChanges) {
    auto &view = followed_.at(layout.tag);
    const bool moved = layout.x != view.layout.x || layout.y != view.layout.y ||
        layout.width != view.layout.width || layout.height != view.layout.height;
    if (moved && layoutAnimationsManager_->getLayoutAnimationConfig(layout.tag, LayoutAnimationType::LAYOUT)) {
      startLayout(layout.tag, view, layout);
      skip[index] = true;
    }
    view.layout = layout;
  }

  for (const auto tag : entering) {
    const auto it = followed_.find(tag);
    if (it != followed_.end() && it->second.hasLayout) {
      startEntering(tag, it->second, appended);
    }
  }

  if constexpr (kSharedTransitions) {
    rootChildren_ = geometry.mountedChildCount(surfaceId_);
    if (refreshBoundaries(geometry)) {
      structure = true;
    }
    if (!containers_.empty() && !progress_ && !pendingPop_) {
      retargetSharedTransitions(geometry);
    }
    if (structure) {
      for (const auto &[tag, _] : removals.deleteOperation) {
        structural_.erase(tag);
      }
      if (pendingPop_) {
        // A finished pop settles when React removes its source screen.
        if (!structural_.contains(pendingPop_->sourceScreen)) {
          pendingPop_.reset();
          topBoundary_ = findTopBoundary();
          restoreTopScreen(appended);
        }
      } else if (!progress_) {
        const auto top = findTopBoundary();
        if (top != topBoundary_) {
          if (topBoundary_ != 0 && top != 0) {
            startSharedTransitions(topBoundary_, top, beforeOrigins, geometry, appended);
          }
          topBoundary_ = top;
          restoreTopScreen(appended);
        }
      }
    }
  }

  // Deleted views that are not held are no longer followed, and nothing may
  // be mounted for them any more: an animation still running on one (an
  // entering or layout animation of a view removed without an exit) stops.
  for (const auto &[tag, index] : removals.deleteOperation) {
    if (!skip[index]) {
      followed_.erase(tag);
      if constexpr (kSharedTransitions) {
        boundaryOf_.erase(tag);
        seen_.erase(tag);
        hidden_.erase(tag);
      }
      if (layoutAnimations_.contains(tag) || hasPendingLayoutAnimation(tag)) {
        cancelLayoutAnimation(tag);
      }
      layoutAnimations_.erase(tag);
      completedAnimations_.erase(tag);
      updateMap_.erase(tag);
    }
  }

  // React's indices in a parent that holds exiting views do not count them:
  // move each removal and insertion there past them, in the order the
  // platform applies them (removals, then insertions). A root held by this
  // batch joins its parent's list where its removal would have run.
  const auto translate = [&](size_t phase) {
    for (const auto index : batch.phaseOperationIndexes[phase]) {
      auto &operation = batch.operations[index];
      if (auto *remove = std::get_if<DirectRemoveOperation>(&operation)) {
        if (skip[index]) {
          if (heldRoots.contains(remove->tag)) {
            auto &siblings = heldInLiveParent_[remove->parentTag];
            // Held views counted before React's child `index` are before it
            // natively; the others are after it. Decide that first, then count
            // it out of React's children for the ones after, as any removal.
            size_t position = 0;
            while (position < siblings.size() && siblings[position].reactBefore <= remove->index) {
              ++position;
            }
            for (size_t later = position; later < siblings.size(); ++later) {
              --siblings[later].reactBefore;
            }
            siblings.insert(
                siblings.begin() + static_cast<std::ptrdiff_t>(position),
                HeldInLiveParent{.tag = remove->tag, .reactBefore = remove->index});
          }
          continue;
        }
        if (heldInLiveParent_.contains(remove->parentTag)) {
          remove->index = nativeRemoveIndex(remove->parentTag, remove->index);
        }
      } else if (auto *insert = std::get_if<DirectInsertOperation>(&operation)) {
        if (!skip[index] && heldInLiveParent_.contains(insert->parentTag)) {
          insert->index = nativeInsertIndex(insert->parentTag, insert->index);
        }
      }
    }
  };
  if (!heldInLiveParent_.empty() || !heldRoots.empty()) {
    translate(LoomMountBatch::Removes);
    translate(LoomMountBatch::Inserts);
  }

  const bool rewritten = !released.operations.empty() || !appended.operations.empty() ||
      std::find(skip.begin(), skip.end(), true) != skip.end();
  if (rewritten) {
    LoomMountBatch out;
    out.requiresTagReplacement = batch.requiresTagReplacement;
    out.animationFrame = batch.animationFrame;
    for (auto &operation : released.operations) {
      out.append(std::move(operation));
    }
    for (size_t index = 0; index < count; ++index) {
      if (!skip[index]) {
        out.append(std::move(batch.operations[index]));
      }
    }
    for (auto &operation : appended.operations) {
      out.append(std::move(operation));
    }
    batch = std::move(out);
  }

  configLock.unlock();
  flushLayoutAnimationOperations(lock);
}

void LoomLayoutAnimationsProxy::frame(
    const SurfaceId /*surfaceId*/,
    LoomMountBatch &batch,
    const LoomLayoutAnimationGeometry &geometry) {
  auto lock = std::unique_lock<std::recursive_mutex>(mutex);

  if constexpr (kSharedTransitions) {
    handleProgressTransition(batch, geometry);
    if (pendingPop_ && pendingPop_->cancelled) {
      // The gesture was cancelled after it finished: the source screen stays.
      for (const auto tag : pendingPop_->sourceViews) {
        restore(tag, batch);
      }
      pendingPop_.reset();
    }
    flushLayoutAnimationOperations(lock);
  }

  for (auto &[tag, values] : updateMap_) {
    // Only views that are still mounted: followed ones, or held while exiting.
    if (!followed_.contains(tag) && !held_.contains(tag)) {
      continue;
    }
    const auto running = layoutAnimations_.find(tag);
    const auto completed = completedAnimations_.find(tag);
    if (running == layoutAnimations_.end() &&
        (completed == completedAnimations_.end() || completed->second.shouldRemove)) {
      continue;
    }
    auto &animation = running != layoutAnimations_.end() ? running->second : completed->second.animation;
    auto view = animation.finalView;
    if (values.newProps) {
      view.props = values.newProps;
    }
    updateLayoutMetrics(view.layoutMetrics, values.frame, animation.frameOffset);

    if (view.props != animation.currentView.props) {
      batch.append(DirectUpdatePropsOperation{.tag = tag, .props = rawPropsOf(view.props), .parsedProps = view.props});
    }
    const auto followedIt = followed_.find(tag);
    if (followedIt != followed_.end() && followedIt->second.hasLayout &&
        view.layoutMetrics.frame != animation.currentView.layoutMetrics.frame) {
      auto layout = followedIt->second.layout;
      layout.x = view.layoutMetrics.frame.origin.x;
      layout.y = view.layoutMetrics.frame.origin.y;
      layout.width = view.layoutMetrics.frame.size.width;
      layout.height = view.layoutMetrics.frame.size.height;
      batch.append(layout);
    }
    animation.currentView = view;
    if (animation.opacity && isViewKind(view) && getViewProps(view).opacity == *animation.opacity) {
      animation.opacity.reset();
    }
  }
  updateMap_.clear();

  // Ended animations; an ended exit mounts the removal it held (and its held
  // ancestors' once their last exiting branch is done).
  for (auto it = completedAnimations_.begin(); it != completedAnimations_.end();) {
    const auto tag = it->first;
    if (hasPendingLayoutAnimation(tag)) {
      ++it;
      continue;
    }
    if (it->second.shouldRemove) {
      if (const auto heldIt = held_.find(tag); heldIt != held_.end() && heldIt->second.exiting) {
        releaseHeld(tag, batch);
      }
    } else {
      reconcileFinalFrame(tag, it->second.animation, batch);
    }
    if constexpr (kSharedTransitions) {
      // A container that arrived: its target shows, the copy goes.
      if (const auto containerIt = containers_.find(tag); containerIt != containers_.end()) {
        restore(containerIt->second.target, batch);
        removeContainer(tag, batch);
      }
    }
    it = completedAnimations_.erase(it);
  }
}

// An animation that ends, finished or not, leaves the view where React laid it
// out and with the opacity an entering animation took over: a cancelled one
// stops at an intermediate frame (Reanimated's cleanupCompletedAnimations).
void LoomLayoutAnimationsProxy::reconcileFinalFrame(
    const Tag tag,
    const LayoutAnimation &animation,
    LoomMountBatch &batch) {
  const auto followedIt = followed_.find(tag);
  if (followedIt == followed_.end() || containers_.contains(tag)) {
    return;
  }
  const auto &view = followedIt->second;
  if (needsFinalFrameReconciliation(animation.type) && view.hasLayout) {
    const auto &frame = animation.currentView.layoutMetrics.frame;
    if (frame.origin.x != view.layout.x || frame.origin.y != view.layout.y ||
        frame.size.width != view.layout.width || frame.size.height != view.layout.height) {
      batch.append(view.layout);
    }
  }
  if (animation.opacity && !hidden_.contains(tag)) {
    setOpacity(tag, *animation.opacity, batch);
  }
}

std::optional<SurfaceId> LoomLayoutAnimationsProxy::endLayoutAnimation(const int tag, const bool shouldRemove) {
  auto lock = std::unique_lock<std::recursive_mutex>(mutex);
  const auto it = layoutAnimations_.find(tag);
  if (it == layoutAnimations_.end()) {
    return {};
  }
  completedAnimations_.insert_or_assign(
      tag, CompletedLayoutAnimation{.animation = it->second, .shouldRemove = shouldRemove});
  layoutAnimations_.erase(it);
  // Reanimated requests a flush for the surface returned here; that frame
  // mounts the final values and, for an exit, the held removal.
  return surfaceId_;
}

// MARK: Shared element transitions

bool LoomLayoutAnimationsProxy::isShared(const Tag tag) const {
  const auto manager = layoutAnimationsManager_->getSharedTransitionManager();
  const auto lock = std::unique_lock<std::mutex>(manager->mutex_);
  return manager->tagToName_.contains(tag);
}

// A boundary is `display: contents`, so it mounts no view and is never in a
// batch: it is found in the renderer, above the first shared view inside it.
Tag LoomLayoutAnimationsProxy::boundaryAbove(const Tag parentTag, const LoomLayoutAnimationGeometry &geometry) {
  for (auto tag = parentTag; tag != 0 && tag != surfaceId_; tag = geometry.parentOf(tag)) {
    if (const auto it = structural_.find(tag); it != structural_.end()) {
      if (it->second.boundary) {
        return tag;
      }
      continue;
    }
    if (const auto *name = geometry.componentNameOf(tag); name != nullptr && *name == kBoundaryComponent) {
      const auto *props = geometry.propsOf(tag);
      auto &entry = structural_[tag];
      entry.boundary = true;
      entry.active = props != nullptr && boundaryIsActive(*props, false);
      entry.screen = screenAbove(geometry.parentOf(tag), geometry);
      return tag;
    }
  }
  return 0;
}

// Boundaries mount nothing, so their `isActive` and their removal are read
// from the renderer. True if any changed.
bool LoomLayoutAnimationsProxy::refreshBoundaries(const LoomLayoutAnimationGeometry &geometry) {
  bool changed = false;
  for (auto it = structural_.begin(); it != structural_.end();) {
    if (!it->second.boundary) {
      ++it;
      continue;
    }
    const auto *props = geometry.propsOf(it->first);
    if (props == nullptr) {
      it = structural_.erase(it);
      changed = true;
      continue;
    }
    const auto active = boundaryIsActive(*props, it->second.active);
    changed = changed || active != it->second.active;
    it->second.active = active;
    ++it;
  }
  return changed;
}

Tag LoomLayoutAnimationsProxy::screenAbove(const Tag parentTag, const LoomLayoutAnimationGeometry &geometry) const {
  for (auto tag = parentTag; tag != 0 && tag != surfaceId_; tag = geometry.parentOf(tag)) {
    if (const auto it = structural_.find(tag); it != structural_.end() && !it->second.boundary) {
      return tag;
    }
  }
  return 0;
}

// React Native Screens lays a screen with a header (its second child) out at
// the top of its stack and moves it below the header natively; Reanimated
// measures shared views the same way.
std::optional<ReactRect> LoomLayoutAnimationsProxy::layoutFrame(
    const Tag tag,
    const LoomLayoutAnimationGeometry &geometry) const {
  auto frame = geometry.layoutFrameInSurface(tag);
  if (!frame) {
    return frame;
  }
  for (auto current = geometry.parentOf(tag); current != 0 && current != surfaceId_;
       current = geometry.parentOf(current)) {
    const auto it = structural_.find(current);
    if (it == structural_.end() || it->second.boundary || geometry.mountedChildCount(current) < 2) {
      continue;
    }
    const auto screen = geometry.layoutFrameInSurface(current);
    const auto stack = geometry.layoutFrameInSurface(geometry.parentOf(current));
    if (screen && stack) {
      frame->origin.y += stack->size.height - screen->size.height;
    }
  }
  return frame;
}

// Reanimated takes the last active boundary in tree order. Screens of a stack
// are mounted in the order they are pushed, so that is the newest one here.
Tag LoomLayoutAnimationsProxy::findTopBoundary() const {
  Tag top = 0;
  for (const auto &[tag, entry] : structural_) {
    if (entry.boundary && entry.active && tag > top) {
      top = tag;
    }
  }
  return top;
}

std::unordered_map<std::string, Tag> LoomLayoutAnimationsProxy::sharedViewsOf(const Tag boundary) const {
  std::unordered_map<std::string, Tag> views;
  if (boundary == 0) {
    return views;
  }
  const auto manager = layoutAnimationsManager_->getSharedTransitionManager();
  const auto lock = std::unique_lock<std::mutex>(manager->mutex_);
  for (const auto &[tag, owner] : boundaryOf_) {
    if (owner != boundary || containers_.contains(tag)) {
      continue;
    }
    const auto nameIt = manager->tagToName_.find(tag);
    if (nameIt == manager->tagToName_.end() || !followed_.contains(tag)) {
      continue;
    }
    auto &slot = views[nameIt->second];
    slot = std::max(slot, tag);
  }
  return views;
}

void LoomLayoutAnimationsProxy::setOpacity(const Tag tag, const std::optional<double> opacity, LoomMountBatch &out) {
  const auto it = followed_.find(tag);
  if (it == followed_.end()) {
    return;
  }
  auto value = 1.0;
  if (opacity) {
    value = *opacity;
  } else if (const auto own = it->second.props.find("opacity");
             own != it->second.props.items().end() && own->second.isNumber()) {
    value = own->second.asDouble();
  }
  const auto shadowView = shadowViewOf(tag, it->second);
  const PropsParserContext context{surfaceId_, *contextContainer_};
  folly::dynamic raw = folly::dynamic::object("opacity", value);
  auto parsed = componentDescriptorRegistry_->at(shadowView.componentHandle)
                    .cloneProps(context, shadowView.props, RawProps(folly::dynamic(raw)));
  out.append(DirectUpdatePropsOperation{.tag = tag, .props = std::move(raw), .parsedProps = std::move(parsed)});
}

void LoomLayoutAnimationsProxy::hide(const Tag tag, LoomMountBatch &out) {
  if (hidden_.insert(tag).second) {
    setOpacity(tag, 0.0, out);
  }
}

void LoomLayoutAnimationsProxy::restore(const Tag tag, LoomMountBatch &out) {
  if (hidden_.erase(tag) > 0) {
    setOpacity(tag, std::nullopt, out);
  }
}

void LoomLayoutAnimationsProxy::restoreTopScreen(LoomMountBatch &out) {
  if (hidden_.empty()) {
    return;
  }
  std::unordered_set<Tag> arriving;
  for (const auto &[_, container] : containers_) {
    arriving.insert(container.target);
  }
  std::vector<Tag> shown;
  for (const auto tag : hidden_) {
    const auto it = boundaryOf_.find(tag);
    if (it != boundaryOf_.end() && it->second == topBoundary_ && !arriving.contains(tag)) {
      shown.push_back(tag);
    }
  }
  for (const auto tag : shown) {
    restore(tag, out);
  }
}

size_t LoomLayoutAnimationsProxy::containerIndex(const Tag tag) const {
  // The surface root holds React's views, the exiting ones it keeps there, and
  // then the containers in the order they were inserted.
  size_t index = rootChildren_;
  if (const auto heldIt = heldInLiveParent_.find(surfaceId_); heldIt != heldInLiveParent_.end()) {
    index += heldIt->second.size();
  }
  const auto position = std::find(containerOrder_.begin(), containerOrder_.end(), tag);
  return index + static_cast<size_t>(position - containerOrder_.begin());
}

Tag LoomLayoutAnimationsProxy::createContainer(
    const Tag source,
    const Tag target,
    const ReactRect &frame,
    const bool progress,
    LoomMountBatch &out) {
  Tag tag = 0;
  {
    const auto manager = layoutAnimationsManager_->getSharedTransitionManager();
    const auto lock = std::unique_lock<std::mutex>(manager->mutex_);
    tag = manager->nextContainerTag_;
    manager->nextContainerTag_ += 2;
  }
  auto view = followed_.at(source);
  if (view.props.isObject()) {
    view.props.erase("nativeID");
  }
  view.parentTag = surfaceId_;
  view.layout.tag = tag;
  view.layout.parentTag = surfaceId_;
  view.layout.x = frame.origin.x;
  view.layout.y = frame.origin.y;
  view.layout.width = frame.size.width;
  view.layout.height = frame.size.height;
  view.hasLayout = true;
  const auto shadowView = shadowViewOf(tag, view);
  out.append(DirectCreateOperation{
      .tag = tag,
      .layoutable = true,
      .componentName = view.componentName,
      .props = view.props,
      .parsedProps = shadowView.props});
  containerOrder_.push_back(tag);
  flyingContainers_.store(containerOrder_.size(), std::memory_order_relaxed);
  out.append(DirectInsertOperation{.parentTag = surfaceId_, .tag = tag, .index = containerIndex(tag)});
  out.append(view.layout);
  followed_[tag] = std::move(view);
  containers_[tag] = Container{.source = source, .target = target, .progress = progress};
  return tag;
}

void LoomLayoutAnimationsProxy::removeContainer(const Tag tag, LoomMountBatch &out) {
  if (!containers_.contains(tag)) {
    return;
  }
  out.append(DirectRemoveOperation{.parentTag = surfaceId_, .tag = tag, .index = containerIndex(tag)});
  out.append(DirectDeleteOperation{.tag = tag});
  containerOrder_.erase(std::find(containerOrder_.begin(), containerOrder_.end(), tag));
  flyingContainers_.store(containerOrder_.size(), std::memory_order_relaxed);
  containers_.erase(tag);
  followed_.erase(tag);
  updateMap_.erase(tag);
}

void LoomLayoutAnimationsProxy::startSharedTransitions(
    const Tag before,
    const Tag after,
    const std::unordered_map<Tag, ReactPoint> &beforeOrigins,
    const LoomLayoutAnimationGeometry &geometry,
    LoomMountBatch &out) {
  const auto targets = sharedViewsOf(after);
  if (targets.empty()) {
    return;
  }
  for (const auto &[name, sourceTag] : sharedViewsOf(before)) {
    const auto targetIt = targets.find(name);
    if (targetIt == targets.end()) {
      continue;
    }
    const auto targetTag = targetIt->second;
    const auto config =
        layoutAnimationsManager_->getLayoutAnimationConfig(sourceTag, LayoutAnimationType::SHARED_ELEMENT_TRANSITION);
    const auto &source = followed_.at(sourceTag);
    const auto &target = followed_.at(targetTag);
    if (!config || !source.hasLayout || !target.hasLayout) {
      continue;
    }

    // From where the source is on screen (measured before this batch: it may
    // be on a screen this batch removes), to where the target's layout puts
    // it, offset as it was the last time it was on screen (scroll, header).
    const auto sourceLayout = layoutFrame(sourceTag, geometry);
    std::optional<ReactPoint> sourceOrigin;
    if (const auto it = beforeOrigins.find(sourceTag); it != beforeOrigins.end()) {
      sourceOrigin = it->second;
    } else if (sourceLayout) {
      sourceOrigin = sourceLayout->origin;
    }
    auto targetFrame = layoutFrame(targetTag, geometry);
    if (!sourceOrigin || !targetFrame) {
      continue;
    }
    seen_[sourceTag] =
        Seen{.native = *sourceOrigin, .layout = sourceLayout ? std::optional(sourceLayout->origin) : std::nullopt};
    if (const auto seenIt = seen_.find(targetTag); seenIt != seen_.end() && seenIt->second.layout) {
      targetFrame->origin += seenIt->second.native - *seenIt->second.layout;
    }
    const ReactRect sourceFrame{*sourceOrigin, {source.layout.width, source.layout.height}};

    // A copy still flying to or from either view gives way to this one.
    std::vector<Tag> superseded;
    for (const auto &[tag, container] : containers_) {
      if (container.source == sourceTag || container.target == sourceTag || container.target == targetTag) {
        superseded.push_back(tag);
      }
    }
    for (const auto tag : superseded) {
      const auto oldTarget = containers_.at(tag).target;
      cancelLayoutAnimation(tag);
      completedAnimations_.erase(tag);
      removeContainer(tag, out);
      if (oldTarget != sourceTag && oldTarget != targetTag) {
        restore(oldTarget, out);
      }
    }

    hide(sourceTag, out);
    hide(targetTag, out);
    const auto containerTag = createContainer(sourceTag, targetTag, sourceFrame, false, out);
    containers_.at(containerTag).targetFrame = *targetFrame;
    auto targetView = target;
    targetView.layout.x = targetFrame->origin.x;
    targetView.layout.y = targetFrame->origin.y;
    targetView.layout.width = targetFrame->size.width;
    targetView.layout.height = targetFrame->size.height;
    enqueueLayoutAnimation(ManagedLayoutAnimationStart{
        .tag = containerTag,
        .type = LayoutAnimationType::SHARED_ELEMENT_TRANSITION,
        .before = shadowViewOf(containerTag, followed_.at(containerTag)),
        .after = shadowViewOf(containerTag, targetView),
        .parentTag = surfaceId_,
        .config = config,
    });
  }
}

// A screen that was just pushed is laid out again once React Native Screens
// reports its frame (below the header): a copy on its way to it follows.
void LoomLayoutAnimationsProxy::retargetSharedTransitions(const LoomLayoutAnimationGeometry &geometry) {
  for (auto &[tag, container] : containers_) {
    if (container.progress || !followed_.contains(container.target)) {
      continue;
    }
    auto frame = layoutFrame(container.target, geometry);
    if (!frame) {
      continue;
    }
    if (const auto seenIt = seen_.find(container.target); seenIt != seen_.end() && seenIt->second.layout) {
      frame->origin += seenIt->second.native - *seenIt->second.layout;
    }
    if (*frame == container.targetFrame) {
      continue;
    }
    const auto config =
        layoutAnimationsManager_->getLayoutAnimationConfig(container.source, LayoutAnimationType::SHARED_ELEMENT_TRANSITION);
    if (!config) {
      continue;
    }
    container.targetFrame = *frame;
    auto targetView = followed_.at(container.target);
    targetView.layout.x = frame->origin.x;
    targetView.layout.y = frame->origin.y;
    targetView.layout.width = frame->size.width;
    targetView.layout.height = frame->size.height;
    const auto after = shadowViewOf(tag, targetView);
    if (hasPendingLayoutAnimation(tag)) {
      updateLayoutAnimationTarget(tag, after, config);
    } else if (const auto animationIt = layoutAnimations_.find(tag); animationIt != layoutAnimations_.end()) {
      enqueueLayoutAnimation(ManagedLayoutAnimationStart{
          .tag = tag,
          .type = LayoutAnimationType::SHARED_ELEMENT_TRANSITION,
          .before = animationIt->second.currentView,
          .after = after,
          .parentTag = surfaceId_,
          .config = config,
      });
    }
  }
}

void LoomLayoutAnimationsProxy::handleProgressTransition(
    LoomMountBatch &out,
    const LoomLayoutAnimationGeometry &geometry) {
  if (!progress_ || !progress_->updated) {
    return;
  }
  auto &transition = *progress_;
  transition.updated = false;
  using State = ProgressTransition::State;

  if (transition.state == State::Start) {
    // The screen coming back shows its newest boundary, active or not yet.
    Tag targetBoundary = 0;
    for (const auto &[tag, entry] : structural_) {
      if (entry.boundary && entry.screen == transition.targetScreen && tag > targetBoundary) {
        targetBoundary = tag;
      }
    }
    const auto sourceIt = structural_.find(topBoundary_);
    if (targetBoundary == 0 || sourceIt == structural_.end() || sourceIt->second.screen != transition.sourceScreen ||
        targetBoundary == topBoundary_) {
      transition.state = State::Cancelled;
    } else {
      const auto targets = sharedViewsOf(targetBoundary);
      for (const auto &[name, sourceTag] : sharedViewsOf(topBoundary_)) {
        const auto targetIt = targets.find(name);
        if (targetIt == targets.end()) {
          continue;
        }
        const auto targetTag = targetIt->second;
        // The target screen sits under the source; where its view is on screen
        // was measured when the source screen covered it.
        const auto seenIt = seen_.find(targetTag);
        const auto sourceOrigin = geometry.nativeOriginInSurface(sourceTag);
        const auto &source = followed_.at(sourceTag);
        const auto &target = followed_.at(targetTag);
        if (seenIt == seen_.end() || !sourceOrigin || !source.hasLayout || !target.hasLayout) {
          continue;
        }
        hide(sourceTag, out);
        hide(targetTag, out);
        const ReactRect sourceFrame{*sourceOrigin, {source.layout.width, source.layout.height}};
        const auto containerTag = createContainer(sourceTag, targetTag, sourceFrame, true, out);
        auto targetView = target;
        targetView.layout.x = seenIt->second.native.x;
        targetView.layout.y = seenIt->second.native.y;
        enqueueLayoutAnimation(ProgressLayoutAnimationStart{
            .tag = containerTag,
            .before = shadowViewOf(containerTag, followed_.at(containerTag)),
            .after = shadowViewOf(containerTag, targetView),
            .parentTag = surfaceId_,
        });
      }
      transition.state = State::Active;
      return;
    }
  }

  if (transition.state == State::Active) {
    for (const auto &[tag, container] : containers_) {
      if (!container.progress || hasPendingLayoutAnimation(tag)) {
        continue;
      }
      const auto animationIt = layoutAnimations_.find(tag);
      if (animationIt == layoutAnimations_.end()) {
        continue;
      }
      const auto &from = animationIt->second.startView.layoutMetrics.frame;
      const auto &to = animationIt->second.finalView.layoutMetrics.frame;
      const auto mix = [&](double a, double b) { return a + transition.progress * (b - a); };
      updateMap_.insert_or_assign(
          tag,
          UpdateValues{
              nullptr,
              Frame(
                  mix(from.origin.x, to.origin.x),
                  mix(from.origin.y, to.origin.y),
                  mix(from.size.width, to.size.width),
                  mix(from.size.height, to.size.height))});
    }
    return;
  }

  // Ended or cancelled: the screen coming back shows its views again. After a
  // finished pop the source's stay hidden until React removes their screen.
  std::vector<Tag> sources;
  std::vector<Tag> finished;
  for (const auto &[tag, container] : containers_) {
    if (container.progress) {
      finished.push_back(tag);
    }
  }
  for (const auto tag : finished) {
    const auto container = containers_.at(tag);
    restore(container.target, out);
    sources.push_back(container.source);
    cancelLayoutAnimation(tag);
    removeContainer(tag, out);
  }
  if (transition.state == State::End) {
    pendingPop_ = PendingPop{.sourceScreen = transition.sourceScreen, .sourceViews = std::move(sources)};
  } else {
    for (const auto tag : sources) {
      restore(tag, out);
    }
  }
  progress_.reset();
}

std::optional<SurfaceId> LoomLayoutAnimationsProxy::onTransitionProgress(
    const int tag,
    const double progress,
    const bool isClosing,
    const bool isGoingForward) {
  if constexpr (!kSharedTransitions) {
    return {};
  }
#ifdef ANDROID
  // Reanimated drives Android's transitions from the commit that pops a screen.
  return {};
#else
  // The screen that comes back while another one is popped.
  if (isClosing || isGoingForward) {
    return {};
  }
  const auto lock = std::unique_lock<std::recursive_mutex>(mutex);
  using State = ProgressTransition::State;
  if (progress_ && (progress_->targetScreen != tag || progress_->state == State::Cancelled)) {
    return {};
  }
  if (!progress_) {
    const auto targetIt = structural_.find(tag);
    if (targetIt == structural_.end() || targetIt->second.boundary || progress >= 1 || pendingPop_) {
      return {};
    }
    const auto topIt = structural_.find(topBoundary_);
    if (topIt == structural_.end() || topIt->second.screen == 0 || topIt->second.screen == tag) {
      return {};
    }
    progress_ = ProgressTransition{.sourceScreen = topIt->second.screen, .targetScreen = tag};
  }
  progress_->progress = progress;
  if ((progress_->state == State::Start || progress_->state == State::Active) && progress >= 1) {
    progress_->state = State::End;
  }
  progress_->updated = true;
  return surfaceId_;
#endif
}

std::optional<SurfaceId> LoomLayoutAnimationsProxy::onGestureCancel(const int tag) {
  if constexpr (!kSharedTransitions) {
    return {};
  }
  const auto lock = std::unique_lock<std::recursive_mutex>(mutex);
  if (pendingPop_ && pendingPop_->sourceScreen == tag) {
    if (pendingPop_->cancelled) {
      return {};
    }
    pendingPop_->cancelled = true;
    return surfaceId_;
  }
  if (!progress_ || progress_->state == ProgressTransition::State::Cancelled || progress_->sourceScreen != tag) {
    return {};
  }
  progress_->state = ProgressTransition::State::Cancelled;
  progress_->updated = true;
  return surfaceId_;
}

} // namespace reanimated::loom
