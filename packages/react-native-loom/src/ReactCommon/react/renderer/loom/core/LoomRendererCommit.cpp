/* Copyright (c) Meta Platforms, Inc. and affiliates. */

// LoomRenderer: the commit pipeline. Preparing nodes, layout, building the mount batch
// (finishCommitAfterLayout, flush), surface failure, and constraints.

#include "LoomRendererShared.h"

#if LOOM_RENDERER
namespace facebook::react {

namespace {

bool samePreparedTextContent(
    const AttributedString& left,
    const AttributedString& right);

bool samePreparedTextContent(
    const std::shared_ptr<const AttributedString>& left,
    const std::shared_ptr<const AttributedString>& right) {
  if (left == right) {
    return true;
  }
  if (!left || !right) {
    return false;
  }
  return samePreparedTextContent(*left, *right);
}

bool samePreparedTextContent(
    const AttributedString& left,
    const AttributedString& right) {
  if (left.getBaseTextAttributes() != right.getBaseTextAttributes()) {
    return false;
  }
  const auto& leftFragments = left.getFragments();
  const auto& rightFragments = right.getFragments();
  if (leftFragments.size() != rightFragments.size()) {
    return false;
  }
  for (size_t index = 0; index < leftFragments.size(); ++index) {
    const auto& leftFragment = leftFragments[index];
    const auto& rightFragment = rightFragments[index];
    if (!leftFragment.isContentEqual(rightFragment) ||
        leftFragment.parentShadowView.tag !=
            rightFragment.parentShadowView.tag) {
      return false;
    }
  }
  return true;
}

} // namespace

void LoomRenderer::prepare(
    Node& node,
    bool insideText,
    bool strictLayout,
    LayoutParent parentView,
    std::vector<Tag>& traversal,
    std::vector<LayoutParent>& layoutParents,
    std::vector<Node*>& nodes) {
  traversal.push_back(node.tag);
  nodes.push_back(&node);
  node.visitedStamp = commitStamp_;
  const bool hadPrepared = node.preparedOnce;
  const bool wasVirtual = node.virtualNode;
  const bool wasLayoutOnly = node.layoutOnlyNode;
  node.prepareSelf = false;
  node.prepareDescendant = false;
  node.preparedOnce = true;
  node.lastInsideText = insideText;
  node.lastParentViewTag = parentView.tag;
  node.inheritedStrictLayout = strictLayout;
  // A LayoutConformance node sets the errata of itself and its subtree
  // (YogaLayoutableShadowNode::resolveErrata); strict is its default mode.
  node.strictLayout = node.layoutConformance
      ? stringProp(node.props, "mode") != "compatibility"
      : strictLayout;

  if (!node.roleFlagsValid) {
    refreshRoleFlags(node);
  }
  const bool text = node.roleIsText;
  const bool paragraph = node.roleIsParagraph;
  const bool textInput = node.roleIsTextInput;

  node.textLike = text;
  if (node.text && !text && !paragraph && !textInput) {
    // The node stopped being text (its component changed); drop the block.
    node.text.reset();
    node.hasTextMeasurementCache = false;
  }
  node.inlineAttachment = insideText && !node.hostOwned && !text;
  node.textRoot = !node.hostOwned && (paragraph || textInput) && !insideText;
  node.virtualNode = !node.hostOwned &&
      (node.inlineAttachment || insideText || (text && !node.textRoot));
  node.layoutOnlyNode =
      !node.hostOwned && !node.virtualNode && node.roleIsLayoutOnlyView;
  // A role change moves this node in or out of its host parent's mounted
  // list (or flattens its children into it); the parent must re-derive it.
  if (hadPrepared &&
      (wasVirtual != node.virtualNode || wasLayoutOnly != node.layoutOnlyNode)) {
    if (auto* parentNode = find(node.parent); parentNode != nullptr) {
      parentNode->childrenDirty = true;
    }
  }
  const bool nextHostLayoutable = !node.virtualNode && !node.layoutOnlyNode;
  if (node.hostCreated && node.hostLayoutable != nextHostLayoutable) {
    emitDelete(node.tag);
    node.hostCreated = false;
    node.replacesHostTagThisCommit = true;
    node.hasMountedLayout = false;
    node.childrenDirty = true;
  }
  layoutParents.push_back(parentView);

  const bool childrenInsideText = insideText || node.textRoot;
  syncYogaNode(node);
  if (node.yogaNode != nullptr) {
    const auto config = node.strictLayout ? strictYogaConfig_ : yogaConfig_;
    if (YGNodeGetConfig(node.yogaNode) != config) {
      // Yoga dirties the node itself when the errata differ.
      YGNodeSetConfig(node.yogaNode, config);
    }
  }
  // Children first: attaching them requires their Yoga nodes to exist.
  auto childParentView = parentView;
  if (!node.virtualNode && !node.layoutOnlyNode) {
    childParentView = LayoutParent{node.tag, 0, 0};
  }
  for (const Tag childTag : node.children) {
    if (auto* rawTextChild = findRawText(childTag); rawTextChild != nullptr) {
      rawTextChild->visitedStamp = commitStamp_;
      continue;
    }
    if (auto* child = find(childTag); child != nullptr) {
      // An unchanged subtree prepared in the same context keeps everything
      // this walk would compute for it, so it is not visited at all.
      if (!fullPrepare_ && !child->prepareSelf && !child->prepareDescendant &&
          child->preparedOnce &&
          child->lastInsideText == childrenInsideText &&
          child->inheritedStrictLayout == node.strictLayout &&
          child->lastParentViewTag == childParentView.tag) {
        continue;
      }
      prepare(
          *child,
          childrenInsideText,
          node.strictLayout,
          childParentView,
          traversal,
          layoutParents,
          nodes);
    }
  }
  if (!node.virtualNode) {
    syncYogaChildren(node);
  }
  if (!node.hostOwned && !node.textRoot &&
      node.roleRequiresIntrinsicMeasurement &&
      !(node.compat != nullptr && node.compat->measurable()) &&
      findProp(node.props, "width") == nullptr &&
      findProp(node.props, "height") == nullptr &&
      !node.intrinsicMeasurementReported) {
    if (operations_.reportUnsupportedIntrinsicMeasurement) {
      operations_.reportUnsupportedIntrinsicMeasurement(
          surfaceId_, node.tag, node.componentName);
    }
    node.intrinsicMeasurementReported = true;
  }
  if (node.textRoot && node.textDirty) {
    buildTextContent(node);
  }
  if (!node.hostOwned && !node.textRoot &&
      node.roleRequiresGenericStateWrapper && node.compat == nullptr &&
      !node.genericStateInitialized) {
    node.needsState = true;
    // Deduplicated by the set itself.
    stateUpdates_.push_back(node.tag);
  }
}

bool LoomRenderer::isSurfaceActive() const {
  return !surfaceFailed_ &&
      (!operations_.isSurfaceActive || operations_.isSurfaceActive(surfaceId_));
}

void LoomRenderer::failSurface(const std::string& reason) noexcept {
  if (surfaceFailed_) {
    return;
  }
  surfaceFailed_ = true;
  insideCommit_ = false;
  commitPending_ = false;
  fullPrepare_ = true;
  stateUpdates_.clear();
  pendingOperations_.clear();
  layoutEvents_.clear();
  removedMountedEdges_.clear();
  removedMountedEdgeKeys_.clear();
  removedNodeTags_.clear();
  removedNodeTagSet_.clear();
  try {
    if (operations_.failSurface) {
      operations_.failSurface(surfaceId_, reason);
    }
  } catch (const std::exception& error) {
    std::fprintf(
        stderr,
        "LOOM_RENDERER failed to stop surface %d after commit error: %s\n",
        static_cast<int>(surfaceId_),
        error.what());
  } catch (...) {
    std::fprintf(
        stderr,
        "LOOM_RENDERER failed to stop surface %d after commit error\n",
        static_cast<int>(surfaceId_));
  }
}

void LoomRenderer::abortCommit(const std::string& reason) noexcept {
  failSurface(reason);
}

bool LoomRenderer::hasFailed() const {
  return surfaceFailed_;
}

Tag LoomRenderer::findUnsupportedInlineAttachment(
    const std::vector<Tag>& rootChildren,
    const std::function<LoomComponentTable::Roles(Tag)>& rolesFor,
    const std::function<const std::vector<Tag>* (Tag)>& childrenFor) {
  TagHashSet visited;
  std::function<Tag(Tag, bool)> visit;
  visit = [&](Tag tag, bool insideText) -> Tag {
    if (!visited.insert(tag).second) {
      return 0;
    }
    // A tag this commit does not know has no roles and no children, so it
    // cannot be an unsupported attachment.
    const auto roles = rolesFor(tag);
    const bool paragraph = roles.isParagraph();
    if (insideText && !roles.isTextStack()) {
      return tag;
    }
    if (roles.isRawText()) {
      return 0;
    }
    const auto* children = childrenFor(tag);
    if (children == nullptr) {
      return 0;
    }
    for (const auto childTag : *children) {
      if (const auto unsupported =
              visit(childTag, insideText || paragraph || roles.textInput);
          unsupported != 0) {
        return unsupported;
      }
    }
    return 0;
  };

  for (const auto rootTag : rootChildren) {
    if (const auto unsupported = visit(rootTag, false); unsupported != 0) {
      return unsupported;
    }
  }
  return 0;
}

void LoomRenderer::collectUnreachable() {
  // An unmount releases every event target in the removed subtrees; the host
  // gets them in one call after the sweep instead of one call per node.
  std::vector<Tag> released;
  releasedEventTargets_ = &released;
  try {
    sweepUnreachable();
  } catch (...) {
    releasedEventTargets_ = nullptr;
    unregisterEventTargets(std::move(released));
    throw;
  }
  releasedEventTargets_ = nullptr;
  unregisterEventTargets(std::move(released));
}

void LoomRenderer::sweepUnreachable() {
  auto* root = find(rootTag_);
  if (root == nullptr) {
    return;
  }
  if (!fullPrepare_) {
    // Nodes leave the tree only by being detached (or never attached), and
    // each of those records itself as a candidate; anything still without a
    // parent now is the root of an unreachable subtree. A candidate that was
    // re-attached elsewhere has a parent again and is skipped.
    auto candidates = std::exchange(orphanCandidates_, {});
    for (const Tag tag : candidates) {
      if (tag == rootTag_ || !exists(tag) || parentOfAny(tag) != 0) {
        continue;
      }
      if (const auto* node = find(tag); node != nullptr && node->hostOwned) {
        continue;
      }
      dropSubtree(tag);
    }
    // dropSubtree detaches nodes, which notes them again; they are gone now.
    orphanCandidates_.clear();
    return;
  }
  orphanCandidates_.clear();
  std::vector<Tag> unreachable;
  for (const auto& entry : nodes_) {
    if (entry.second->visitedStamp != commitStamp_) {
      unreachable.push_back(entry.first);
    }
  }
  for (const auto& entry : rawTextLeaves_) {
    if (entry.second.visitedStamp != commitStamp_) {
      unreachable.push_back(entry.first);
    }
  }
  for (const Tag tag : unreachable) {
    auto* node = find(tag);
    if (node != nullptr && node->hostOwned) {
      continue;
    }
    // Only the roots of the unreachable forest are walked; the rest are
    // reached through their parents. React has no explicit delete, so
    // unreachability from the root is the only signal there is.
    const auto parentTag = parentOfAny(tag);
    if (parentTag != 0 && exists(parentTag) &&
        !wasVisitedThisCommit(parentTag)) {
      continue;
    }
    dropSubtree(tag);
  }
  orphanCandidates_.clear();
}

bool LoomRenderer::reachesRoot(Tag tag) const {
  for (size_t steps = 0; tag != 0 && steps <= nodes_.size(); ++steps) {
    if (tag == rootTag_) {
      return true;
    }
    tag = parentOfAny(tag);
  }
  return false;
}

bool LoomRenderer::wasVisitedThisCommit(Tag tag) const {
  if (const auto* node = find(tag); node != nullptr) {
    return node->visitedStamp == commitStamp_;
  }
  if (const auto* leaf = findRawText(tag); leaf != nullptr) {
    return leaf->visitedStamp == commitStamp_;
  }
  return false;
}

void LoomRenderer::commit() {
  // A commit requested while one is already running is deferred, not nested: the
  // running commit drains it (see the `commitPending_` handling in
  // `finishCommitAfterLayout`) so the reentrant work is coalesced into a single
  // follow-up commit. Continuing here as well would run a second commit inside
  // the first, and the deferred one would then be a third.
  if (!beginCommitLayout()) {
    return;
  }
  if (!commitPrepared_)
    return;
  calculateCommitLayout();
  finishCommitAfterLayout();
}

namespace {
// Below these, a commit is small enough that the hand-off costs more than the
// overlap gains.
constexpr size_t kEarlyCreateMinimum = 48;
constexpr size_t kParallelTextMinimum = 12;
} // namespace

void LoomRenderer::emitEarlyCreates() {
  if (!operations_.applyBatchEarly ||
      !LoomSettings::enabled(LoomSettingId::EarlyCreate)) {
    return;
  }
  const auto earlyCreatable = [](const Node& node) {
    return !node.hostCreated && !node.hostOwned &&
        !node.replacesHostTagThisCommit;
  };
  size_t count = 0;
  for (const auto* node : commitNodes_) {
    count += earlyCreatable(*node) ? 1 : 0;
  }
  if (count < kEarlyCreateMinimum) {
    return;
  }
  // Creates carry everything a view needs before layout: its component and its
  // props. Whatever `prepare` already queued (deletes of re-roled nodes) stays
  // in this commit's own batch, and so do re-roled nodes themselves: their old
  // incarnation's delete has to be applied in the same batch as their create.
  auto queued = std::move(pendingOperations_);
  pendingOperations_.clear();
  for (auto* node : commitNodes_) {
    if (earlyCreatable(*node)) {
      emitCreate(*node);
    }
  }
  auto early = std::move(pendingOperations_);
  pendingOperations_ = std::move(queued);
  operations_.applyBatchEarly(surfaceId_, std::move(early));
}

void LoomRenderer::speculateTextMeasurements() {
  if ((!operations_.prepareText && !operations_.measureText) ||
      !LoomSettings::enabled(LoomSettingId::ParallelText)) {
    return;
  }
  const auto& parallelFor = LoomPlatformTraits::current().parallelFor;
  if (!parallelFor) {
    return;
  }
  // For new paragraphs only (a first screen, a navigation): text that changes
  // in views that already exist is measured once per update by the real pass,
  // and an extra estimating pass plus speculative measurements on every update
  // cost far more CPU than the parallelism saves.
  size_t unmeasured = 0;
  for (const auto* node : commitNodes_) {
    if (node->textRoot && node->yogaNode != nullptr &&
        (node->createdThisCommit || !node->hostCreated) &&
        !(node->hasTextMeasurementCache &&
          node->measuredContentGeneration ==
              node->measurementContentGeneration)) {
      ++unmeasured;
    }
  }
  if (unmeasured < kParallelTextMinimum) {
    return;
  }
  // 1. Lay out with estimated text sizes, recording the constraints Yoga asks
  //    each paragraph for. They are usually the real ones: a paragraph's width
  //    comes from its ancestors, rarely from the size of other text.
  std::vector<SpeculativeTextRequest> requests;
  requests.reserve(unmeasured * 2);
  speculativeTextRequests_ = &requests;
  try {
    layoutTree();
  } catch (...) {
    speculativeTextRequests_ = nullptr;
    throw;
  }
  speculativeTextRequests_ = nullptr;
  const auto sameRequest = [](const SpeculativeTextRequest& left,
                              const SpeculativeTextRequest& right) {
    return left.node == right.node && left.minimumSize == right.minimumSize &&
        left.maximumSize == right.maximumSize;
  };
  std::vector<SpeculativeTextRequest> unique;
  unique.reserve(requests.size());
  for (const auto& request : requests) {
    bool seen = false;
    for (auto index = unique.size(); index-- > 0 && !seen;) {
      if (unique[index].node != request.node) {
        break; // Yoga asks for one node's sizes back to back.
      }
      seen = sameRequest(unique[index], request);
    }
    if (!seen) {
      unique.push_back(request);
    }
  }
  // 2. Measure them for real, in parallel. The first one runs alone so lazily
  //    created platform text state comes up on one thread.
  std::vector<TextMeasurement> results(unique.size());
  if (!unique.empty()) {
    results[0] = measureTextWithPlatform(
        *unique[0].node, unique[0].minimumSize, unique[0].maximumSize);
  }
  if (unique.size() > 1) {
    parallelFor(unique.size() - 1, [&](size_t index) {
      const auto& request = unique[index + 1];
      results[index + 1] = measureTextWithPlatform(
          *request.node, request.minimumSize, request.maximumSize);
    });
  }
  for (size_t index = 0; index < unique.size(); ++index) {
    if (results[index].valid) {
      storeTextMeasurement(
          *unique[index].node,
          unique[index].minimumSize,
          unique[index].maximumSize,
          results[index]);
    }
  }
  // 3. Every paragraph Yoga sized from an estimate is measured again by the
  //    real pass; constraints that match hit the entries stored above.
  for (const auto& request : unique) {
    YGNodeMarkDirty(request.node->yogaNode);
  }
}

bool LoomRenderer::beginCommitLayout() {
  if (insideCommit_) {
    commitPending_ = true;
    return false;
  }
  auto* root = find(rootTag_);
  if (root == nullptr)
    return false;
  insideCommit_ = true;
  commitPrepared_ = false;
  try {
    using WorkerProfile = LoomWorkerProfile;
    workerCommitStart_ = WorkerProfile::enabled()
        ? WorkerProfile::Clock::now()
        : WorkerProfile::Clock::time_point{};
#if LOOM_PROFILE
    profileCommitStart_ = std::chrono::steady_clock::now();
    profileTextMeasureCalls_ = 0;
    profileTextMeasureCacheHits_ = 0;
    profileTextMeasureNanoseconds_ = 0;
    profileStateUpdateCalls_ = 0;
    profileStateUpdateNanoseconds_ = 0;
    profileApplyBatchNanoseconds_ = 0;
    profilePreparedTextStateCount_ = 0;
    profileTextSpannableBuildNanos_ = 0;
    profileTextPaintSetupNanos_ = 0;
    profileTextLayoutCreateNanos_ = 0;
    profileTextIsBoringNanos_ = 0;
    profileTextBoringMakeNanos_ = 0;
    profileTextMeasurementResultNanos_ = 0;
    profileSimplePlainTextLayouts_ = 0;
    profileTextBoringLayouts_ = 0;
    profileTextStaticLayouts_ = 0;
#endif
    unsupportedInlineAttachment_ = false;
    // Opening a new commit invalidates every previous mark at once. The counter
    // starts above zero, and zero is what an untouched node still carries, so a
    // node that was reached in commit k is not mistaken for one reached in k+1.
    ++commitStamp_;
    // No eager reserve: the vectors keep their capacity between commits, and
    // reserving the whole tree would defeat the trim for small commits. The
    // trim runs at the end of each commit (`releaseSlack`), so a large commit's
    // capacity is not held until the next one.
    commitTraversal_.clear();
    commitNodes_.clear();
    commitLayoutParents_.clear();
    prepare(
        *root,
        false,
        false,
        LayoutParent{root->tag, 0, 0},
        commitTraversal_,
        commitLayoutParents_,
        commitNodes_);
    workerPreparedAt_ = WorkerProfile::enabled()
        ? WorkerProfile::Clock::now()
        : WorkerProfile::Clock::time_point{};
#if LOOM_PROFILE
    profileAfterPrepare_ = std::chrono::steady_clock::now();
#endif
    commitPrepared_ = true;
    emitEarlyCreates();
  } catch (const std::exception& error) {
    insideCommit_ = false;
    failSurface(error.what());
    throw;
  } catch (...) {
    insideCommit_ = false;
    failSurface("unknown exception during Loom renderer prepare");
    throw;
  }
  return true;
}

void LoomRenderer::calculateCommitLayout() {
  if (!commitPrepared_)
    return;
  try {
    speculateTextMeasurements();
    layoutTree();
    workerLaidOutAt_ = LoomWorkerProfile::enabled()
        ? LoomWorkerProfile::Clock::now()
        : LoomWorkerProfile::Clock::time_point{};
#if LOOM_PROFILE
    profileAfterLayout_ = std::chrono::steady_clock::now();
#endif
  } catch (const std::exception& error) {
    insideCommit_ = false;
    commitPrepared_ = false;
    failSurface(error.what());
    throw;
  } catch (...) {
    insideCommit_ = false;
    commitPrepared_ = false;
    failSurface("unknown exception during Loom renderer layout");
    throw;
  }
}

void LoomRenderer::finishCommitAfterLayout() {
  if (!commitPrepared_)
    return;
  try {
    using WorkerProfile = LoomWorkerProfile;
    const bool workerProfiling = WorkerProfile::enabled();
    if (unsupportedInlineAttachment_) {
      // Do not mount a misleading attachment character or a partial tree. The
      // Loom renderer has no safe generic inline-child host contract; leave
      // the previous host tree intact and fail the surface explicitly. Silently
      // dropping the commit would let React believe it was applied while the
      // host keeps its previous contents, so every later commit carrying the
      // attachment would be dropped the same way.
      pendingOperations_.clear();
      stateUpdates_.clear();
      layoutEvents_.clear();
      insideCommit_ = false;
      commitPrepared_ = false;
      failSurface(
          "Loom renderer commit contains an unsupported inline text attachment");
      if (operations_.commitFinished) {
        operations_.commitFinished(surfaceId_);
      }
      return;
    }
    hasCommittedLayout_ = true;

    for (auto* node : commitNodes_) {
      if (!node->hostCreated) {
        emitCreate(*node);
      } else {
        emitUpdateProps(*node);
      }
    }

    // Materialize the view-child order once and retain a linear slice per
    // traversal node. Child identity maps and an ordered scan identify an
    // already-correct subsequence; only removed or moved children need host
    // operations. This avoids rebuilding child vectors and repeatedly searching
    // the wide mounted/next lists for each individual child.
    // Scratch lives on the renderer: these were four heap vectors plus a hash
    // set sized to the whole tree on every commit. Trimmed after the diff.
    auto& nextMountedChildren = scratchMounted_;
    auto& childStarts = scratchChildStarts_;
    auto& childCounts = scratchChildCounts_;
    auto& childOrderUnchanged = scratchChildUnchanged_;
    auto& stationaryChildren = scratchStationary_;
    nextMountedChildren.clear();
    childStarts.assign(commitTraversal_.size(), 0);
    childCounts.assign(commitTraversal_.size(), 0);
    childOrderUnchanged.assign(commitTraversal_.size(), 1);
    stationaryChildren.clear();
    const auto appendMounted = [&](const auto& self,
                                   const Node& parent) -> void {
      for (const Tag childTag : parent.children) {
        const auto* child = find(childTag);
        if (child == nullptr || child->virtualNode) {
          continue;
        }
        if (child->layoutOnlyNode) {
          self(self, *child);
        } else {
          nextMountedChildren.push_back(childTag);
        }
      }
    };

    // A flattened (layout-only) node's children are mounted in its host
    // ancestor, so a change in its list is a change in that ancestor's list.
    // Children come after parents in the traversal, so walk it backwards.
    if (!fullPrepare_) {
      for (size_t index = commitNodes_.size(); index-- > 0;) {
        auto* node = commitNodes_[index];
        if (node->childrenDirty && node->layoutOnlyNode) {
          if (auto* parentNode = find(node->parent); parentNode != nullptr) {
            parentNode->childrenDirty = true;
          }
        }
      }
    }
    for (size_t traversalIndex = 0; traversalIndex < commitTraversal_.size();
         ++traversalIndex) {
      auto* node = commitNodes_[traversalIndex];
      childStarts[traversalIndex] = nextMountedChildren.size();
      if (!fullPrepare_ && !node->childrenDirty) {
        // Same children, same roles: the mounted list is still correct.
        childCounts[traversalIndex] = 0;
        continue;
      }
      if (!node->virtualNode && !node->layoutOnlyNode) {
        appendMounted(appendMounted, *node);
      }
      // Fabric's mount order is the stable order of the flattened child list
      // by orderIndex (ViewProps::zIndex). UIKit hit testing walks siblings in
      // reverse subview order, so preserving React child order here while
      // ignoring zIndex can leave a visually-frontmost sibling behind another
      // view for both drawing and touch targeting.
      const auto childStart = childStarts[traversalIndex];
      const auto childEnd = nextMountedChildren.size();
      // Nearly every sibling group has no zIndex at all; the stable sort of an
      // all-zero key leaves React order unchanged, so check before decorating.
      bool anyZIndex = false;
      if (childEnd - childStart > 1) {
        for (auto index = childStart; index < childEnd && !anyZIndex; ++index) {
          auto* child = find(nextMountedChildren[index]);
          if (child != nullptr && !child->roleFlagsValid) {
            refreshRoleFlags(*child);
          }
          anyZIndex = child != nullptr && child->roleZIndex != 0.0f;
        }
      }
      if (anyZIndex) {
        // Decorate once rather than looking up both nodes and both prop maps on
        // every comparison. That comparator resolved a hash entry for each tag and
        // then a dynamic-map entry for `zIndex` - four lookups per comparison, so
        // a sibling group of n cost about 4*n*log(n) of them. No node's zIndex
        // changes while this sort runs, so one pass is enough. The sort stays
        // stable, so equal zIndex values keep React child order exactly as before,
        // and a missing zIndex keeps sorting as 0.
        const auto childCount = static_cast<size_t>(childEnd - childStart);
        std::vector<std::pair<float, Tag>> ordered;
        ordered.reserve(childCount);
        for (auto index = childStart; index < childEnd; ++index) {
          const auto tag = nextMountedChildren[index];
          auto* child = find(tag);
          if (child != nullptr && !child->roleFlagsValid) {
            refreshRoleFlags(*child);
          }
          ordered.emplace_back(child == nullptr ? 0.0f : child->roleZIndex, tag);
        }
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });
        for (size_t index = 0; index < childCount; ++index) {
          nextMountedChildren[childStart + index] = ordered[index].second;
        }
      }
      const auto nextCount =
          nextMountedChildren.size() - childStarts[traversalIndex];
      childCounts[traversalIndex] = nextCount;
      const auto* next = nextMountedChildren.empty()
          ? nullptr
          : nextMountedChildren.data() + childStarts[traversalIndex];
      const auto& mounted = node->mountedChildren;

      if (mounted.size() == nextCount &&
          (nextCount == 0 ||
           std::equal(mounted.begin(), mounted.end(), next))) {
        continue;
      }
      childOrderUnchanged[traversalIndex] = 0;

      TagMap<size_t> oldIndices;
      oldIndices.reserve(mounted.size());
      for (size_t index = 0; index < mounted.size(); ++index) {
        oldIndices.emplace(mounted[index], index);
      }
      // Retain an increasing subsequence of children already in the requested
      // order. A single forward scan is sufficient: any child whose old index
      // is not greater than the last retained index is moved. This is linear
      // after the identity map and avoids a quadratic search/erase/insert
      // simulation.
      size_t lastStationaryIndex = 0;
      bool hasStationaryChild = false;
      for (size_t index = 0; index < nextCount; ++index) {
        const auto old = oldIndices.find(next[index]);
        if (old == oldIndices.end()) {
          continue;
        }
        const auto oldIndex = old->second;
        if (!hasStationaryChild || oldIndex > lastStationaryIndex) {
          stationaryChildren.insert(childPairKey(node->tag, next[index]));
          lastStationaryIndex = oldIndex;
          hasStationaryChild = true;
        }
      }
    }

    initialInsertIndexes_.clear();
    // Edges owned by removed parents were captured from the previous live tree.
    // Emit descendant edges first, before removing an ancestor or inserting a
    // moved child elsewhere.
    const bool rootRemovesOnly =
        LoomPlatformTraits::current().removesSubtreeRootOnly;
    for (const auto& edge : removedMountedEdges_) {
      // `removesSubtreeRootOnly`: an edge inside a subtree this commit
      // unmounts goes with its parent. Both ends must be leaving the tree; a
      // child that moves elsewhere is still reachable and keeps its remove.
      if (rootRemovesOnly && !reachesRoot(edge.parentTag) &&
          !reachesRoot(edge.childTag)) {
        continue;
      }
      pendingOperations_.append(
          DirectRemoveOperation{edge.parentTag, edge.childTag, edge.index});
    }
    // Remove non-stationary children in descending old-index order so every
    // emitted index refers to the hierarchy state at that point in the batch.
    for (size_t index = 0; index < commitTraversal_.size(); ++index) {
      if (childOrderUnchanged[index] != 0) {
        continue;
      }
      emitRemovals(*commitNodes_[index], stationaryChildren);
    }
    for (size_t traversalIndex = 0; traversalIndex < commitTraversal_.size();
         ++traversalIndex) {
      if (childOrderUnchanged[traversalIndex] != 0) {
        continue;
      }
      auto* node = commitNodes_[traversalIndex];
      const auto count = childCounts[traversalIndex];
      const auto* next = count == 0
          ? nullptr
          : nextMountedChildren.data() + childStarts[traversalIndex];
      emitInsertions(*node, next, count, stationaryChildren);
    }
    trimScratch(scratchChildStarts_);
    trimScratch(scratchChildCounts_);
    trimScratch(scratchChildUnchanged_);
    // Overflow bounds are a post-layout, bottom-up property. Cache one result
    // per node so nested subtrees are not rescanned once for every ancestor. The
    // memo lives on the node rather than in a tag-keyed map: the map cost an
    // O(tree) bucket array plus one allocated entry per node per commit, to hold
    // a value the node can carry itself. `commitStamp_` is what makes a cached
    // value distinguishable from the previous commit's.
    std::function<std::array<float, 4>(Tag)> calculateOverflow =
        [&](Tag tag) -> std::array<float, 4> {
      auto* owner = find(tag);
      if (owner == nullptr) {
        return {};
      }
      if (owner->overflowStamp == commitStamp_) {
        return owner->commitOverflow;
      }
      // Overflow depends only on the subtree's geometry and props. A node the
      // post-layout walk did not reach has neither, so its last result holds.
      if (owner->layoutVisitStamp != commitStamp_ && owner->overflowStamp != 0) {
        return owner->commitOverflow;
      }
      std::array<float, 4> result{};
      if (!owner->roleFlagsValid) {
        refreshRoleFlags(*owner);
      }
      if (owner->yogaNode == nullptr || owner->roleClipsOverflow) {
        owner->commitOverflow = result;
        owner->overflowStamp = commitStamp_;
        return result;
      }
      const auto ownerWidth = YGNodeLayoutGetWidth(owner->yogaNode);
      const auto ownerHeight = YGNodeLayoutGetHeight(owner->yogaNode);
      float minX = 0;
      float minY = 0;
      float maxX = 0;
      float maxY = 0;
      for (const auto childTag : owner->children) {
        const auto* child = find(childTag);
        if (child == nullptr || child->yogaNode == nullptr ||
            child->displayNone) {
          continue;
        }
        const auto childOverflow = calculateOverflow(childTag);
        const auto childLeft = YGNodeLayoutGetLeft(child->yogaNode);
        const auto childTop = YGNodeLayoutGetTop(child->yogaNode);
        const auto childWidth = YGNodeLayoutGetWidth(child->yogaNode);
        const auto childHeight = YGNodeLayoutGetHeight(child->yogaNode);
        minX = std::min(minX, childLeft + childOverflow[0]);
        minY = std::min(minY, childTop + childOverflow[1]);
        maxX = std::max(maxX, childLeft + childWidth - childOverflow[2]);
        maxY = std::max(maxY, childTop + childHeight - childOverflow[3]);
      }
      result = {
          std::min(minX, 0.0f),
          std::min(minY, 0.0f),
          -std::max(maxX - ownerWidth, 0.0f),
          -std::max(maxY - ownerHeight, 0.0f)};
      owner->commitOverflow = result;
      owner->overflowStamp = commitStamp_;
      return result;
    };
    layoutNodes_.clear();
    layoutNodeParents_.clear();
    if (auto* layoutRoot = find(rootTag_); layoutRoot != nullptr) {
      collectLayoutNodes(*layoutRoot, LayoutParent{layoutRoot->tag, 0, 0}, false);
    }
    for (size_t index = 0; index < layoutNodes_.size(); ++index) {
      auto* node = layoutNodes_[index];
      if (!node->virtualNode) {
        emitLayout(
            *node, layoutNodeParents_[index], calculateOverflow(node->tag));
      }
    }
    for (auto* node : layoutNodes_) {
      if (node->yogaNode != nullptr) {
        YGNodeSetHasNewLayout(node->yogaNode, false);
      }
    }
    for (size_t index = 0; index < layoutNodes_.size(); ++index) {
      auto* node = layoutNodes_[index];
      if (node->componentName != scrollViewName_ ||
          node->yogaNode == nullptr) {
        continue;
      }
      Rect contentBounds{};
      bool hasContentBounds = false;
      for (const Tag childTag : node->children) {
        const auto* child = find(childTag);
        if (child == nullptr || child->yogaNode == nullptr ||
            child->displayNone) {
          continue;
        }
        const Rect childBounds{
            .origin =
                {YGNodeLayoutGetLeft(child->yogaNode),
                 YGNodeLayoutGetTop(child->yogaNode)},
            .size = {
                YGNodeLayoutGetWidth(child->yogaNode),
                YGNodeLayoutGetHeight(child->yogaNode)}};
        if (!hasContentBounds) {
          contentBounds = childBounds;
          hasContentBounds = true;
        } else {
          contentBounds.unionInPlace(childBounds);
        }
      }
      if (!node->hasScrollContentBounds ||
          node->mountedScrollContentBounds != contentBounds) {
        if (operations_.updateScrollContentBounds) {
          operations_.updateScrollContentBounds(
              surfaceId_, node->tag, node->props, contentBounds);
        }
        node->mountedScrollContentBounds = contentBounds;
        node->hasScrollContentBounds = true;
      }
    }
    // Old nodes were intentionally not copied into the speculative revision.
    // Emit their deletes only after the selected tree has completed layout, in
    // postorder captured from the old live tree (children before parents).
    for (const Tag tag : removedNodeTags_) {
      emitDelete(tag);
    }
    collectUnreachable();
    // Keep captured edges alive through collectUnreachable(): dropping a
    // removed parent walks its old children too, but those edges were already
    // emitted above. dropSubtree uses this list to avoid duplicate native
    // removes.
    removedMountedEdges_.clear();
    removedMountedEdgeKeys_.clear();
    removedNodeTags_.clear();
    removedNodeTagSet_.clear();
    releaseSlack();
#if LOOM_PROFILE
    const auto profileAfterDiff = std::chrono::steady_clock::now();
    const auto profileOperationCount = pendingOperations_.operations.size();
    const auto profileStateCount = stateUpdates_.size();
#endif
    const auto workerDiffed = workerProfiling
        ? WorkerProfile::Clock::now()
        : WorkerProfile::Clock::time_point{};
    flush();
    fullPrepare_ = false;
    if (workerProfiling) {
      auto& metrics = WorkerProfile::metrics();
      metrics.prepare +=
          WorkerProfile::ms(workerCommitStart_, workerPreparedAt_);
      metrics.layout += WorkerProfile::ms(workerPreparedAt_, workerLaidOutAt_);
      metrics.diff += WorkerProfile::ms(workerLaidOutAt_, workerDiffed);
      metrics.effects +=
          WorkerProfile::ms(workerDiffed, WorkerProfile::Clock::now());
    }
#if LOOM_PROFILE
    const auto profileAfterFlush = std::chrono::steady_clock::now();
#endif
    insideCommit_ = false;

    // A host callback may synchronously stop this surface. Its renderer object
    // is kept alive by the binding until this stack unwinds, but it must not
    // publish later layout events or drain stale reentrant work.
    if (!isSurfaceActive()) {
      commitPending_ = false;
      layoutEvents_.clear();
      return;
    }

    if (commitPending_) {
      commitPending_ = false;
      commit();
      return;
    }
    if (operations_.commitFinished) {
      operations_.commitFinished(surfaceId_);
    }
#if LOOM_PROFILE
    if (LoomWorkerProfile::enabled()) {
      const auto profileMs = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
      };
      loomLog(
          LoomLogLevel::Profile,
          "LoomRendererProfile",
          "surface=%d total=%.3fms prepare=%.3fms layout=%.3fms diff=%.3fms "
          "flush=%.3fms textMeasure(inPrepare)=%llu/%.3fms cacheHits=%llu "
          "stateCallback(inFlush)=%llu/%.3fms applyBatch(inFlush)=%.3fms "
          "preparedText=%llu boring=%llu static=%llu simplePlain=%llu "
          "spannableBuild=%.3fms paintSetup=%.3fms layoutCreate=%.3fms "
          "isBoring=%.3fms boringMake=%.3fms measureResult=%.3fms "
          "ops=%llu states=%llu",
          static_cast<int>(surfaceId_),
          profileMs(profileCommitStart_, profileAfterFlush),
          profileMs(profileCommitStart_, profileAfterPrepare_),
          profileMs(profileAfterPrepare_, profileAfterLayout_),
          profileMs(profileAfterLayout_, profileAfterDiff),
          profileMs(profileAfterDiff, profileAfterFlush),
          static_cast<unsigned long long>(profileTextMeasureCalls_),
          static_cast<double>(profileTextMeasureNanoseconds_) / 1.0e6,
          static_cast<unsigned long long>(profileTextMeasureCacheHits_),
          static_cast<unsigned long long>(profileStateUpdateCalls_),
          static_cast<double>(profileStateUpdateNanoseconds_) / 1.0e6,
          static_cast<double>(profileApplyBatchNanoseconds_) / 1.0e6,
          static_cast<unsigned long long>(profilePreparedTextStateCount_),
          static_cast<unsigned long long>(profileTextBoringLayouts_),
          static_cast<unsigned long long>(profileTextStaticLayouts_),
          static_cast<unsigned long long>(profileSimplePlainTextLayouts_),
          static_cast<double>(profileTextSpannableBuildNanos_) / 1.0e6,
          static_cast<double>(profileTextPaintSetupNanos_) / 1.0e6,
          static_cast<double>(profileTextLayoutCreateNanos_) / 1.0e6,
          static_cast<double>(profileTextIsBoringNanos_) / 1.0e6,
          static_cast<double>(profileTextBoringMakeNanos_) / 1.0e6,
          static_cast<double>(profileTextMeasurementResultNanos_) / 1.0e6,
          static_cast<unsigned long long>(profileOperationCount),
          static_cast<unsigned long long>(profileStateCount));
    }
#endif
    commitPrepared_ = false;
  } catch (const std::exception& error) {
    insideCommit_ = false;
    commitPrepared_ = false;
    failSurface(error.what());
    throw;
  } catch (...) {
    insideCommit_ = false;
    commitPrepared_ = false;
    failSurface("unknown exception during Loom renderer diff");
    throw;
  }
}

void LoomRenderer::flush() {
  // Queue every changed StateWrapper before the single native-to-Java call.
  // Java associates state with creates (the normal create(state) contract) and
  // applies state for existing views after the ordered mount operations.
  bool hasPendingState = false;
  for (const Tag tag : stateUpdates_) {
    if (!isSurfaceActive()) {
      stateUpdates_.clear();
      pendingOperations_.clear();
      layoutEvents_.clear();
      return;
    }
    auto* node = find(tag);
    if (node == nullptr || !node->needsState) {
      continue;
    }
    hasPendingState = true;
    node->needsState = false;
    if (node->compat != nullptr) {
      // The component's own state object, so the host view and the library's
      // native code see exactly what Fabric would have mounted.
      if (operations_.updateComponentState && node->compat->state()) {
        operations_.updateComponentState(
            surfaceId_, tag, node->compat->state());
      }
      node->compat->stateDirty = false;
      node->genericStateInitialized = true;
      continue;
    }
    if (operations_.updateState) {
#if LOOM_PROFILE
      const auto profileStateStart = std::chrono::steady_clock::now();
#endif
      LoomTextMeasurement preparedState;
      const auto& text = node->textOrEmpty();
      const auto& candidate = text.measuredText.prepared;
      if (node->textRoot && node->yogaNode != nullptr) {
        const auto expectedWidth = YGNodeLayoutGetRawWidth(node->yogaNode) -
            node->mountedContentInsets[0] - node->mountedContentInsets[2];
        const auto expectedHeight = YGNodeLayoutGetRawHeight(node->yogaNode) -
            node->mountedContentInsets[1] - node->mountedContentInsets[3];
        constexpr float layoutEpsilon = 0.01f;
        const bool measurementMatchesCurrentText =
            node->hasTextMeasurementCache &&
            sameTextMeasurementContent(
                text.measuredAttributedString, text.attributedString) &&
            text.measuredParagraphAttributes == text.paragraphAttributes &&
            std::abs(text.measuredPointScaleFactor - pointScaleFactor_) <=
                0.0001f;
        const bool preparedMatchesCurrentText = candidate.valid &&
            candidate.preparedLayout &&
            samePreparedTextContent(text.preparedAttributedString,
                                    text.attributedString) &&
            text.preparedParagraphAttributes == text.paragraphAttributes;
        if (measurementMatchesCurrentText && preparedMatchesCurrentText &&
            std::abs(candidate.size.width - expectedWidth) <= layoutEpsilon &&
            std::abs(candidate.size.height - expectedHeight) <= layoutEpsilon) {
          preparedState = candidate;
#if LOOM_PROFILE
          ++profilePreparedTextStateCount_;
#endif
        } else if (
            operations_.prepareTextForMountProvidesLayout &&
            std::isfinite(expectedWidth) && std::isfinite(expectedHeight) &&
            expectedWidth >= 0.0f && expectedHeight >= 0.0f) {
          // The text's paint attributes may change without changing its Yoga
          // measurement. Refresh the drawable layout against the final content
          // box without asking Yoga to measure the paragraph again.
          const Size finalContentSize{expectedWidth, expectedHeight};
          if (operations_.prepareTextForMount) {
            preparedState = operations_.prepareTextForMount(
                surfaceId_,
                Node::TextData::stringOf(text.attributedString),
                text.paragraphAttributes,
                finalContentSize,
                finalContentSize);
            if (preparedState.valid && preparedState.preparedLayout) {
              preparedState.size = finalContentSize;
              preparedState.minimumSize = finalContentSize;
              preparedState.maximumSize = finalContentSize;
            } else {
              preparedState = {};
            }
          }
          if ((!preparedState.valid || !preparedState.preparedLayout) &&
              operations_.prepareText) {
            auto measuredPreparedState = operations_.prepareText(
                surfaceId_,
                Node::TextData::stringOf(text.attributedString),
                text.paragraphAttributes,
                finalContentSize,
                finalContentSize);
            if (measuredPreparedState.valid &&
                measuredPreparedState.preparedLayout &&
                std::abs(measuredPreparedState.size.width - expectedWidth) <=
                    layoutEpsilon &&
                std::abs(measuredPreparedState.size.height - expectedHeight) <=
                    layoutEpsilon) {
              preparedState = std::move(measuredPreparedState);
            }
          }
          if (preparedState.valid && preparedState.preparedLayout) {
            auto& textMutable = node->ensureText();
            textMutable.measuredText.prepared = preparedState;
            textMutable.preparedAttributedString = text.attributedString;
            textMutable.preparedParagraphAttributes = text.paragraphAttributes;
#if LOOM_PROFILE
            ++profilePreparedTextStateCount_;
#endif
          }
        }
      }
      if (!LoomPlatformTraits::current().hasPreparedTextLayout &&
          !preparedState.valid && node->textRoot && node->yogaNode != nullptr) {
        // Hosts without a prepared layout still need the final content box: it
        // is the size the paragraph view lays its text out in.
        preparedState.size = Size{
            std::max(0.0f, YGNodeLayoutGetRawWidth(node->yogaNode) -
                               node->mountedContentInsets[0] -
                               node->mountedContentInsets[2]),
            std::max(0.0f, YGNodeLayoutGetRawHeight(node->yogaNode) -
                               node->mountedContentInsets[1] -
                               node->mountedContentInsets[3])};
      }
      if (!node->roleFlagsValid) {
        refreshRoleFlags(*node);
      }
      LoomTextStateUpdate update;
      update.surfaceId = surfaceId_;
      update.tag = tag;
      update.kind = node->roleStateKind;
      if (update.kind != LoomTextStateKind::Other) {
        // Shared with the node, not copied.
        update.attributedString = text.attributedString
            ? text.attributedString
            : std::make_shared<const AttributedString>();
        update.paragraphAttributes = text.paragraphAttributes;
      }
      if (update.kind == LoomTextStateKind::TextInput) {
        if (const auto* count = findProp(node->props, "mostRecentEventCount");
            count != nullptr && (count->isInt() || count->isDouble())) {
          update.mostRecentEventCount = count->asInt();
        }
      }
      update.prepared = std::move(preparedState);
      operations_.updateState(std::move(update));
#if LOOM_PROFILE
      profileStateUpdateNanoseconds_ += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - profileStateStart)
              .count());
      ++profileStateUpdateCalls_;
#endif
    }
    if (!node->textRoot) {
      node->genericStateInitialized = true;
    }
  }
  if (!isSurfaceActive()) {
    stateUpdates_.clear();
    pendingOperations_.clear();
    layoutEvents_.clear();
    return;
  }
  if (!pendingOperations_.operations.empty() || hasPendingState) {
    if (operations_.applyBatch) {
#if LOOM_PROFILE
      const auto applyBatchStart = std::chrono::steady_clock::now();
#endif
      pendingOperations_.animationFrame = animationFrameCommit_;
      operations_.applyBatch(surfaceId_, std::move(pendingOperations_));
#if LOOM_PROFILE
      profileApplyBatchNanoseconds_ += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - applyBatchStart)
              .count());
#endif
    }
    pendingOperations_.clear();
  }
  if (!isSurfaceActive()) {
    stateUpdates_.clear();
    layoutEvents_.clear();
    return;
  }
  stateUpdates_.clearAndTrim();
  initialInsertIndexes_.clear();
  initialInsertIndexes_.releaseSlack();
  // Only nodes this commit's prepare walk reached can carry these marks: every
  // write that sets one also marks the node for that walk. A full commit
  // reached every live node, so this covers what the whole-map loop did.
  for (auto* node : commitNodes_) {
    node->createdThisCommit = false;
    node->replacesHostTagThisCommit = false;
    node->childrenDirty = false;
  }
  // The traversal is dead now. Trimmed against this commit's size, so a large
  // commit followed by a small one (a mount, then an unmount) gives the memory
  // back at the end of the small one instead of holding it until the next.
  trimScratch(commitTraversal_);
  trimScratch(commitNodes_);
  trimScratch(commitLayoutParents_);

  // Batch, text state, and layout notifications form one synchronous UI-thread
  // commit boundary. No observable layout callback escapes before all three
  // pieces are installed.
  emitLayoutEvents();
}

void LoomRenderer::setConstraints(
    float minWidth,
    float minHeight,
    float maxWidth,
    float maxHeight,
    bool isRTL) {
  minWidth = std::isfinite(minWidth) && minWidth > 0 ? minWidth : 0;
  minHeight = std::isfinite(minHeight) && minHeight > 0 ? minHeight : 0;
  maxWidth = std::isfinite(maxWidth) && maxWidth > 0 ? maxWidth : 0;
  maxHeight = std::isfinite(maxHeight) && maxHeight > 0 ? maxHeight : 0;
  if (minWidth == minWidth_ && minHeight == minHeight_ &&
      maxWidth == maxWidth_ && maxHeight == maxHeight_ && isRTL == isRTL_) {
    return;
  }
  minWidth_ = minWidth;
  minHeight_ = minHeight;
  maxWidth_ = maxWidth;
  maxHeight_ = maxHeight;
  isRTL_ = isRTL;
#if LOOM_PROFILE
  if (LoomWorkerProfile::enabled()) {
    // TEMPORARY DIAGNOSTIC: what viewport did the host actually hand us?
    loomLog(
        LoomLogLevel::Profile,
        "LoomConstraints",
        "surface=%d minW=%.1f minH=%.1f maxW=%.1f maxH=%.1f rtl=%d",
        static_cast<int>(surfaceId_),
        minWidth_,
        minHeight_,
        maxWidth_,
        maxHeight_,
        isRTL_ ? 1 : 0);
  }
#endif
  // A finite bound on either axis is a valid Yoga measure input. A zero/zero
  // update is the host's provisional unmeasured viewport; do not reflow an
  // already committed tree against it, and do not publish a first tree until
  // at least one real bound exists.
  const bool hasCurrentBound =
      minWidth_ > 0 || minHeight_ > 0 || maxWidth_ > 0 || maxHeight_ > 0;
  if (!hasCurrentBound) {
    return;
  }
  reflow();
}

bool LoomRenderer::setPointScaleFactor(float pointScaleFactor) {
  pointScaleFactor = pointScaleFactor > 0 ? pointScaleFactor : 1.0f;
  if (std::abs(pointScaleFactor_ - pointScaleFactor) <= 0.0001f) {
    return false;
  }
  pointScaleFactor_ = pointScaleFactor;
  YGConfigSetPointScaleFactor(yogaConfig_, pointScaleFactor_);
  YGConfigSetPointScaleFactor(strictYogaConfig_, pointScaleFactor_);
  laidOutMinWidth_ = -1;
  laidOutMinHeight_ = -1;
  laidOutMaxWidth_ = -1;
  laidOutMaxHeight_ = -1;
  for (auto& entry : nodes_) {
    entry.second->hasTextMeasurementCache = false;
  }
  fullPrepare_ = true;
  return true;
}

void LoomRenderer::reflow() {
  commit();
}

} // namespace facebook::react
#endif // LOOM_RENDERER
