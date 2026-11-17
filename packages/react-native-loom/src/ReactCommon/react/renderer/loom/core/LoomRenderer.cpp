/* Copyright (c) Meta Platforms, Inc. and affiliates. */

// LoomRenderer: construction, the node tree (create, children, identity, drop), native
// props, animation frames, commands and accessors.
// The rest of the class lives beside this file:
//   LoomRendererProps.cpp   props, style and role flags
//   LoomRendererCommit.cpp  the commit pipeline: prepare, layout, flush
//   LoomRendererEmit.cpp    mount operations and layout events
//   LoomRendererText.cpp, LoomRendererTree.cpp, LoomRendererDOM.cpp, LoomRendererMetrics.cpp

#include "LoomRendererShared.h"

#if LOOM_RENDERER
namespace facebook::react {

LoomRenderer::LoomRenderer(
    SurfaceId surfaceId,
    NativeOperations operations)
    : surfaceId_(surfaceId),
      operations_(std::move(operations)),
      scrollViewName_(
          LoomPlatformTraits::current().components.hostName("ScrollView")),
      yogaConfig_(YGConfigNew()),
      strictYogaConfig_(YGConfigNew()) {
  // A power-of-two bucket count keeps `nodes_` on mask-based indexing (see TagHash).
  nodes_.reserve(1024);
  rawTextLeaves_.reserve(256);
  // Match YogaLayoutableShadowNode::initializeYogaConfig and its legacy
  // compatibility configuration used by the normal Fabric renderer.
  YGConfigSetUseWebDefaults(yogaConfig_, false);
  YGConfigSetErrata(yogaConfig_, YGErrataAll);
  YGConfigSetExperimentalFeatureEnabled(
      yogaConfig_,
      YGExperimentalFeatureFixFlexBasisFitContent,
      ReactNativeFeatureFlags::fixYogaFlexBasisFitContentInMainAxis());
  // YogaLayoutableShadowNode::resolveErrata for LayoutConformance::Strict.
  YGConfigSetUseWebDefaults(strictYogaConfig_, false);
  YGConfigSetErrata(
      strictYogaConfig_,
      ReactNativeFeatureFlags::enableFlexboxAutoMinSizeInStrictMode()
          ? YGErrataNone
          : YGErrataMinSizeUndefinedInsteadOfAuto);
  YGConfigSetExperimentalFeatureEnabled(
      strictYogaConfig_,
      YGExperimentalFeatureFixFlexBasisFitContent,
      ReactNativeFeatureFlags::fixYogaFlexBasisFitContentInMainAxis());
}

LoomRenderer::~LoomRenderer() {
  // A surface can stop without React sending a delete for every retained node.
  // Event targets are surface-owned and must be removed at teardown, otherwise
  // a later tag reuse can dispatch into the old surface.
  if (isSurfaceActive() && operations_.unregisterEventTarget) {
    for (const auto& entry : nodes_) {
      if (entry.second->hasEventTarget) {
        operations_.unregisterEventTarget(surfaceId_, entry.first);
      }
    }
  }
  for (auto& entry : nodes_) {
    if (entry.second->yogaNode != nullptr) {
      YGNodeFree(entry.second->yogaNode);
      entry.second->yogaNode = nullptr;
    }
  }
  // Shapes own Yoga template nodes; release them while the config is alive.
  shapeCache_.clear();
  for (auto& entry : nodes_) {
    entry.second->shape.reset();
  }
  YGConfigFree(yogaConfig_);
  YGConfigFree(strictYogaConfig_);
}

void LoomRenderer::detachFromParent(Node& node) {
  if (node.parent == 0) {
    return;
  }
  if (auto* parent = find(node.parent); parent != nullptr) {
    const auto oldSize = parent->children.size();
    parent->children.erase(
        std::remove(parent->children.begin(), parent->children.end(), node.tag),
        parent->children.end());
    parent->childrenDirty =
        parent->childrenDirty || parent->children.size() != oldSize;
    markNeedsPrepare(*parent);
    // A move can target a new parent whose subtree is prepared before the old
    // parent is visited (or the old parent may itself be removed). Detach the
    // Yoga edge here, while the old owner is still known, so Yoga never sees a
    // child inserted into two parents.
    if (parent->yogaNode != nullptr && node.yogaNode != nullptr) {
      const auto yogaChild = std::find(
          parent->yogaChildren.begin(), parent->yogaChildren.end(), node.tag);
      if (yogaChild != parent->yogaChildren.end()) {
        YGNodeRemoveChild(parent->yogaNode, node.yogaNode);
        parent->yogaChildren.erase(yogaChild);
      }
    }
  }
  node.parent = 0;
  noteOrphanCandidate(node.tag);
}

void LoomRenderer::dropSubtree(Tag tag) {
  if (auto* leaf = findRawText(tag); leaf != nullptr) {
    detachRawTextFromParent(*leaf);
    rawTextLeaves_.erase(tag);
    droppedTags_.push_back(tag);
    return;
  }
  auto* node = find(tag);
  if (node == nullptr) {
    return;
  }
  droppedTags_.push_back(tag);
  // Native hierarchy removals are emitted from mountedChildren during the
  // commit diff (including captured edges for removed parents). `node.parent`
  // is the React/Yoga parent and may be a flattened layout-only node, so it is
  // never a reliable Java ViewGroup tag here. Emitting a remove from it can
  // crash when that parent has no native View.
  const auto children = node->children;
  for (const Tag childTag : children) {
    dropSubtree(childTag);
  }
  if (node->hasEventTarget) {
    if (operations_.unregisterEventTarget) {
      operations_.unregisterEventTarget(surfaceId_, tag);
    }
    node->hasEventTarget = false;
  }
  // A node can be reconciled speculatively and removed before any commit ever
  // creates its host entry. In that case there is nothing for the platform to
  // delete (and, on iOS, no component-view descriptor to look up).
  if (node->hostCreated) {
    emitDelete(tag);
  }
  if (node->yogaNode != nullptr) {
    YGNodeFree(node->yogaNode);
    node->yogaNode = nullptr;
  }
  invalidateFindCache(tag);
  nodes_.erase(tag);
}

void LoomRenderer::createNode(
    Tag tag,
    const std::string& componentName,
    const folly::dynamic& props,
    InstanceHandle::Shared instanceHandle) {
  const auto& components = LoomPlatformTraits::current().components;
  if (components.classify(componentName).isRawText()) {
    createRawTextNode(
        tag,
        isFlatProps(props) ? stringProp(props, "text")
                           : stringProp(hoistProps(props), "text"));
    return;
  }

  auto iterator = nodes_.find(tag);
  Node* node = nullptr;
  Tag previousParent = 0;
  if (auto* oldLeaf = findRawText(tag); oldLeaf != nullptr) {
    previousParent = oldLeaf->parent;
    detachRawTextFromParent(*oldLeaf);
    rawTextLeaves_.erase(tag);
  }
  if (iterator == nodes_.end()) {
    auto fresh = std::make_unique<Node>();
    fresh->tag = tag;
    fresh->owner = this;
    fresh->parent = previousParent;
    node = fresh.get();
    nodes_.emplace(tag, std::move(fresh));
  } else {
    node = iterator->second.get();
    if (node->hostCreated) {
      // React reuses a tag for a different component after it was unmounted
      // without an explicit delete; drop what the host knows about it first.
      if (node->hasEventTarget && operations_.unregisterEventTarget) {
        operations_.unregisterEventTarget(surfaceId_, tag);
      }
      emitDelete(tag);
      node->replacesHostTagThisCommit = true;
      node->hostCreated = false;
      node->hasEventTarget = false;
      node->hostOwned = false;
    }
  }

  // The host name maps LayoutConformance to a plain view; keep what it is.
  node->layoutConformance = componentName == "LayoutConformance";
  node->componentName = components.hostName(componentName);
  node->componentRoles = components.classify(node->componentName);
  node->collectPropsDelta = componentSendsPropDiffs(node->componentName);
  if (node->parent != 0) {
    if (auto* parent = find(node->parent); parent != nullptr) {
      parent->childrenDirty = true;
      markNeedsPrepare(*parent);
    }
  }
  if (node->parent == 0) {
    node->parent = previousParent;
  }
  node->hasMountedLayout = false;
  node->mountedParentTag = 0;
  node->hasMountedInsets = false;
  node->hostOwned =
      tag == surfaceId_ || node->componentName == kRootComponentName;
  node->childrenDirty = true;
  node->props = hoistProps(props);
  node->propsDelta = nullptr;
  // Both inputs of the role flags change here, so the cache must not survive.
  node->roleFlagsValid = false;
  ++node->propsGeneration;
  node->propsDirty = true;
  node->styleDirty = true;
  node->textDirty = true;
  node->genericStateInitialized = false;
  node->intrinsicMeasurementReported = false;
  node->shape.reset();
  // A (re)created node starts a new family: its compat ShadowNode, if any, is
  // rebuilt from these props on the next prepare.
  node->compat.reset();
  node->compatDescriptor = nullptr;
  node->compatChecked = false;
  // Offscreen hiding belongs to a version of the node it replaces; a new
  // version starts visible, and the refresh below recomputes displayNone.
  node->offscreenHidden = false;
  refreshStyleCached(*node);

  node->instanceHandle = instanceHandle;
  if (instanceHandle != nullptr && operations_.registerEventTarget) {
    operations_.registerEventTarget(surfaceId_, tag, instanceHandle);
    node->hasEventTarget = true;
  }

  if (node->hostOwned && rootTag_ == 0) {
    rootTag_ = tag;
  }
  // A recreated node is a new version: prepare it and its children afresh.
  node->preparedOnce = false;
  markNeedsPrepare(*node);
  if (node->parent == 0) {
    noteOrphanCandidate(tag);
  }
}

void LoomRenderer::createRawTextNode(Tag tag, std::string text) {
  Tag parent = 0;
  if (auto* oldNode = find(tag); oldNode != nullptr) {
    parent = oldNode->parent;
    if (oldNode->hostCreated) {
      if (oldNode->hasEventTarget && operations_.unregisterEventTarget) {
        operations_.unregisterEventTarget(surfaceId_, tag);
      }
      emitDelete(tag);
    }
    if (oldNode->yogaNode != nullptr) {
      YGNodeFree(oldNode->yogaNode);
    }
    invalidateFindCache(tag);
    nodes_.erase(tag);
  }
  if (auto* oldLeaf = findRawText(tag); oldLeaf != nullptr) {
    parent = oldLeaf->parent;
    detachRawTextFromParent(*oldLeaf);
    rawTextLeaves_.erase(tag);
  }

  RawTextLeaf leaf;
  leaf.tag = tag;
  leaf.parent = parent;
  leaf.text = std::move(text);
  rawTextLeaves_.emplace(tag, std::move(leaf));
  markTextDirty(parent);
  if (parent == 0) {
    noteOrphanCandidate(tag);
  } else if (auto* parentNode = find(parent); parentNode != nullptr) {
    markNeedsPrepare(*parentNode);
  }
}

void LoomRenderer::updateRawText(Tag tag, std::string text) {
  if (auto* leaf = findRawText(tag); leaf != nullptr && leaf->text != text) {
    leaf->text = std::move(text);
    markTextDirty(leaf->parent);
  }
}

void LoomRenderer::setInstanceHandle(
    Tag tag,
    InstanceHandle::Shared instanceHandle) {
  auto* node = find(tag);
  if (node == nullptr || instanceHandle == nullptr) {
    return;
  }
  if (node->instanceHandle == instanceHandle) {
    return;
  }
  if (node->hasEventTarget && operations_.unregisterEventTarget) {
    operations_.unregisterEventTarget(surfaceId_, tag);
    node->hasEventTarget = false;
  }
  node->instanceHandle = std::move(instanceHandle);
  if (operations_.registerEventTarget) {
    operations_.registerEventTarget(surfaceId_, tag, node->instanceHandle);
    node->hasEventTarget = true;
  }
}

uint64_t LoomRenderer::identityOf(Tag tag) const {
  const auto* node = find(tag);
  if (node != nullptr) {
    return node->identity;
  }
  const auto* leaf = findRawText(tag);
  return leaf == nullptr ? 0 : leaf->identity;
}

void LoomRenderer::setIdentity(Tag tag, uint64_t identity) {
  if (auto* node = find(tag); node != nullptr) {
    node->identity = identity;
  } else if (auto* leaf = findRawText(tag); leaf != nullptr) {
    leaf->identity = identity;
  }
}

void LoomRenderer::captureRemovedMountedEdges(
    const TagMap<uint64_t>& selectedIdentities) {
  std::unordered_set<Tag> visited;
  std::function<void(Tag)> visit = [&](Tag parentTag) {
    if (!visited.insert(parentTag).second) {
      return;
    }
    const auto* parent = find(parentTag);
    if (parent == nullptr) {
      return;
    }
    for (const auto childTag : parent->children) {
      visit(childTag);
    }
    const auto selected = selectedIdentities.find(parentTag);
    if (selected != selectedIdentities.end() &&
        selected->second == parent->identity) {
      return;
    }
    const auto& mounted = parent->mountedChildren;
    for (size_t index = mounted.size(); index-- > 0;) {
      const auto childTag = mounted[index];
      // Tags are 32-bit, so the pair fits one key and the lookup stays O(1).
      const auto edgeKey = (static_cast<uint64_t>(parentTag) << 32) |
          static_cast<uint64_t>(childTag);
      if (removedMountedEdgeKeys_.insert(edgeKey).second) {
        removedMountedEdges_.push_back({parentTag, childTag, index});
      }
    }
  };
  if (rootTag_ != 0) {
    visit(rootTag_);
  }
}


void LoomRenderer::captureRemovedMountedEdgesFrom(
    const TagMap<uint64_t>& keptIdentities,
    const std::vector<Tag>& removalRoots) {
  // Same per-node rule and children-first order as the whole-tree walk above,
  // restricted to the subtrees that can actually lose nodes. A kept node ends
  // the walk: its subtree is unchanged, or it is a selected draft whose own
  // removed children are listed as roots separately.
  std::unordered_set<Tag> visited;
  const auto visit = [&](const auto& self, Tag parentTag) -> void {
    const auto* parent = find(parentTag);
    if (parent == nullptr || !visited.insert(parentTag).second) {
      return;
    }
    const auto kept = keptIdentities.find(parentTag);
    if (kept != keptIdentities.end() && kept->second == parent->identity) {
      return;
    }
    for (const auto childTag : parent->children) {
      self(self, childTag);
    }
    const auto& mounted = parent->mountedChildren;
    for (size_t index = mounted.size(); index-- > 0;) {
      const auto childTag = mounted[index];
      const auto edgeKey = (static_cast<uint64_t>(parentTag) << 32) |
          static_cast<uint64_t>(childTag);
      if (removedMountedEdgeKeys_.insert(edgeKey).second) {
        removedMountedEdges_.push_back({parentTag, childTag, index});
      }
    }
  };
  for (const auto root : removalRoots) {
    visit(visit, root);
  }
}

void LoomRenderer::collectLayoutNodes(
    Node& node,
    LayoutParent parentView,
    bool ancestorOffsetChanged) {
  node.layoutVisitStamp = commitStamp_;
  layoutNodes_.push_back(&node);
  layoutNodeParents_.push_back(parentView);
  // A layout-only view is flattened away, so its children are positioned in
  // the host parent through it: if it moved, all of them moved there too. A
  // host view resets that, since its children are positioned inside it.
  bool offsetChanged = false;
  if (node.layoutOnlyNode && node.yogaNode != nullptr) {
    constexpr float epsilon = 0.01f;
    offsetChanged = ancestorOffsetChanged || !node.hasMountedLayout ||
        std::abs(YGNodeLayoutGetLeft(node.yogaNode) - node.mountedLeft) >
            epsilon ||
        std::abs(YGNodeLayoutGetTop(node.yogaNode) - node.mountedTop) > epsilon;
  } else if (node.virtualNode) {
    offsetChanged = ancestorOffsetChanged;
  }
  const auto childParentView = (!node.virtualNode && !node.layoutOnlyNode)
      ? LayoutParent{node.tag, 0, 0}
      : parentView;
  for (const Tag childTag : node.children) {
    auto* child = find(childTag);
    if (child == nullptr || child->yogaNode == nullptr) {
      continue;
    }
    // Yoga only re-lays out dirty nodes and the children of nodes it laid out,
    // flagging exactly those with a new layout; anything else kept its frame.
    if (fullPrepare_ || offsetChanged ||
        child->visitedStamp == commitStamp_ ||
        YGNodeGetHasNewLayout(child->yogaNode)) {
      collectLayoutNodes(*child, childParentView, offsetChanged);
    }
  }
}

void LoomRenderer::cloneNodeWithNewProps(
    Tag tag,
    const folly::dynamic& propsPatch) {
  if (auto* leaf = findRawText(tag); leaf != nullptr) {
    if (const auto* text = findProp(propsPatch, "text"); text != nullptr) {
      leaf->text = text->isString() ? text->getString() : std::string{};
      markTextDirty(leaf->parent);
    }
    return;
  }
  if (auto* node = find(tag); node != nullptr) {
    patchProps(*node, propsPatch);
  }
}


bool LoomRenderer::offscreenHidden(Tag tag) const {
  const auto* node = find(tag);
  return node != nullptr && node->offscreenHidden;
}

void LoomRenderer::setOffscreenHidden(Tag tag, bool hidden) {
  auto* node = find(tag);
  if (node == nullptr || node->offscreenHidden == hidden) {
    return;
  }
  node->offscreenHidden = hidden;
  node->displayNone =
      hidden || stringProp(node->props, "display") == "none";
  node->styleDirty = true;
  markNeedsPrepare(*node);
}

void LoomRenderer::patchProps(
    Node& node,
    const folly::dynamic& propsPatch) {
  // [LoomMemory] Flat patches (nearly all of them) are used in place; `hoistProps`
  // returns by value, so every patch used to be deep-copied first.
  folly::dynamic hoistedStorage;
  const bool flat = isFlatProps(propsPatch);
  if (!flat) {
    hoistedStorage = hoistProps(propsPatch);
  }
  const folly::dynamic& hoisted = flat ? propsPatch : hoistedStorage;
  // A patch of paint-only keys (the allow-list the paint-only commit uses)
  // affects neither Yoga style nor text, so re-parsing the node's whole style
  // and invalidating text would change nothing: skip both. The node conditions
  // are the ones `tryCommitPaintOnlyProps` requires for the same guarantee.
  const bool paintOnly = !node.virtualNode && !node.layoutOnlyNode &&
      !node.componentRoles.isParagraph() &&
      isConservativePaintOnlyPatch(hoisted);
  mergeProps(node, hoisted);
  if (!paintOnly) {
    refreshStyle(node);
    markTextDirty(node.tag);
  }
}


void LoomRenderer::setChildren(
    Tag parentTag,
    const std::vector<Tag>& children) {
  auto* parent = find(parentTag);
  if (parent == nullptr) {
    return;
  }
  // The common case is an unchanged list (the root on every commit, a parent
  // cloned for a child's sake). The current list is already normalized, so an
  // identical request needs no hash set, no copy and no work.
  if (parent->children == children) {
    return;
  }
  std::vector<Tag> normalizedChildren;
  normalizedChildren.reserve(children.size());
  std::unordered_set<Tag> selectedChildren;
  selectedChildren.reserve(children.size());
  for (const Tag childTag : children) {
    if (childTag == parentTag || !exists(childTag) ||
        !selectedChildren.insert(childTag).second) {
      continue;
    }
    normalizedChildren.push_back(childTag);
  }
  if (parent->children == normalizedChildren) {
    return;
  }
  for (const Tag childTag : parent->children) {
    if (selectedChildren.count(childTag) == 0) {
      if (parentOfAny(childTag) == parentTag) {
        if (auto* child = find(childTag); child != nullptr) {
          child->parent = 0;
          noteOrphanCandidate(childTag);
        } else if (auto* leaf = findRawText(childTag); leaf != nullptr) {
          leaf->parent = 0;
          noteOrphanCandidate(childTag);
        }
      }
    }
  }
  parent->children.clear();
  parent->childrenDirty = true;
  markNeedsPrepare(*parent);
  for (const Tag childTag : normalizedChildren) {
    auto* child = find(childTag);
    auto* rawText = findRawText(childTag);
    if (child == nullptr && rawText == nullptr) {
      continue;
    }
    if (parentOfAny(childTag) != parentTag) {
      if (child != nullptr) {
        detachFromParent(*child);
      } else {
        detachRawTextFromParent(*rawText);
      }
    }
    if (child != nullptr) {
      const bool attached = child->parent != parentTag;
      child->parent = parentTag;
      if (attached) {
        // Its context (inside text, host parent) may differ under the new
        // parent; re-prepare it rather than trust the cached answers.
        child->preparedOnce = false;
        markNeedsPrepare(*child);
      }
    } else {
      rawText->parent = parentTag;
    }
    parent->children.push_back(childTag);
    markTextDirty(childTag);
  }
  markTextDirty(parentTag);
}

void LoomRenderer::setNativeProps(
    Tag tag,
    const folly::dynamic& propsPatch) {
  auto* node = find(tag);
  if (node == nullptr) {
    return;
  }
  // Native Animated drives one of these per animated node per frame, so a patch
  // the paint-only path accepts must not pay a full commit. That path merges the
  // props exactly as `patchProps` does and skips re-parsing Yoga style and text,
  // which is sound because its allow-list holds only keys that affect neither -
  // for the nodes it accepts, `refreshStyle` and `markTextDirty` would both be
  // no-ops, so the resulting node state and emitted batch are unchanged.
  //
  // Anything else takes the same path a props diff from React takes, so a patch
  // applied from JS and a patch applied by a commit cannot leave different state
  // behind.
  if (tryCommitPaintOnlyProps({{tag, propsPatch}})) {
    return;
  }
  patchProps(*node, propsPatch);
  commit();
}

void LoomRenderer::completeRoot(
    Tag containerTag,
    const std::vector<Tag>& children) {
  completeRoot(containerTag, children, true);
}

bool LoomRenderer::applyComponentState(const StateUpdate& update) {
  if (update.family == nullptr || !isSurfaceActive()) {
    return false;
  }
  auto* node = find(update.family->getTag());
  if (node == nullptr || node->compat == nullptr ||
      !node->compat->updateState(update)) {
    return false;
  }
  // The new revision ran `adopt`, which may have changed the node's Yoga
  // style; the commit re-copies it, lays out and publishes the new state
  // together with the layout it caused.
  markNeedsPrepare(*node);
  commit();
  return true;
}

void LoomRenderer::completeRoot(
    Tag containerTag,
    const std::vector<Tag>& children,
    bool runCommit) {
  rootTag_ = containerTag;
  if (find(rootTag_) == nullptr) {
    createNode(rootTag_, kRootComponentName, folly::dynamic::object());
  }
  auto* root = find(rootTag_);
  if (root == nullptr) {
    return;
  }
  root->hostOwned = true;
  // The root's children are authoritative and come straight from React, so the
  // surface does not depend on the tag bookkeeping to find its own children.
  setChildren(rootTag_, children);
  if (runCommit) {
    commit();
  }
}

bool LoomRenderer::hasSameInstanceHandle(
    Tag tag,
    const InstanceHandle::Shared& handle) const {
  const auto* node = find(tag);
  return node != nullptr && node->instanceHandle == handle;
}

void LoomRenderer::foldPaintProps(
    const std::vector<std::pair<Tag, folly::dynamic>>& patches) {
  for (const auto& [tag, patch] : patches) {
    auto* node = find(tag);
    if (node == nullptr || !node->hostCreated) {
      continue;
    }
    // No refreshStyle, no markTextDirty and no emission: none of these keys is
    // visible to Yoga and the host already applied the values to the native
    // view (design §3.2). `mergeProps` still marks the node props-dirty, so the
    // next real commit ships the merged set, exactly as a normal patch would.
    if (isFlatProps(patch)) {
      mergeProps(*node, patch);
    } else {
      mergeProps(*node, hoistProps(patch));
    }
  }
}

void LoomRenderer::applyAnimationFrame(
    const std::vector<std::pair<Tag, folly::dynamic>>& paintPatches,
    const std::vector<std::pair<Tag, folly::dynamic>>& layoutPatches) {
  foldPaintProps(paintPatches);
  if (layoutPatches.empty()) {
    return;
  }
  for (const auto& [tag, patch] : layoutPatches) {
    auto* node = find(tag);
    if (node == nullptr) {
      continue;
    }
    // A layout key needs the style rebuild and the text invalidation every
    // other prop write gets, and `commit()` coalesces the layout of every node
    // it reaches into a single batch — which is what makes this one commit per
    // frame however many nodes are animating (design §5.2).
    patchProps(*node, patch);
  }
  // The batch this commit flushes is an animation frame (LoomMountBatch).
  animationFrameCommit_ = true;
  commit();
  animationFrameCommit_ = false;
}

bool LoomRenderer::tryCommitPaintOnlyProps(
    const std::vector<std::pair<Tag, folly::dynamic>>& patches) {
  if (patches.empty() || insideCommit_ || !isSurfaceActive() ||
      !pendingOperations_.operations.empty() || !stateUpdates_.empty() ||
      !layoutEvents_.empty() || !removedMountedEdges_.empty()) {
    return false;
  }

  std::vector<Tag> changedTags;
  std::unordered_set<Tag> seenChangedTags;
  for (const auto& [tag, patch] : patches) {
    const auto* node = find(tag);
    if (node == nullptr || !node->hostCreated || node->hostOwned ||
        node->virtualNode || node->layoutOnlyNode ||
        node->componentRoles.isParagraph() ||
        !isConservativePaintOnlyPatch(patch)) {
      return false;
    }
    if (seenChangedTags.insert(tag).second) {
      changedTags.push_back(tag);
    }
  }

  insideCommit_ = true;
  try {
    // All patches were validated before mutation. Keep the full merged props
    // contract for Java, but do not reparse Yoga style, text, or layout.
    for (const auto& [tag, patch] : patches) {
      auto* node = find(tag);
      mergeProps(*node, hoistProps(patch));
    }
    for (const auto tag : changedTags) {
      if (auto* node = find(tag); node != nullptr) {
        emitUpdateProps(*node);
      }
    }
    if (!pendingOperations_.operations.empty() && operations_.applyBatch) {
      operations_.applyBatch(surfaceId_, std::move(pendingOperations_));
    }
    pendingOperations_.clear();
    insideCommit_ = false;
    if (!isSurfaceActive()) {
      commitPending_ = false;
      return true;
    }
    if (commitPending_) {
      commitPending_ = false;
      commit();
    } else if (operations_.commitFinished) {
      operations_.commitFinished(surfaceId_);
    }
    return true;
  } catch (const std::exception& error) {
    insideCommit_ = false;
    failSurface(error.what());
    throw;
  } catch (...) {
    insideCommit_ = false;
    failSurface("unknown exception during paint-only direct commit");
    throw;
  }
}

void LoomRenderer::dispatchCommand(
    Tag tag,
    const std::string& commandName,
    const folly::dynamic& args) {
  if (operations_.dispatchCommand) {
    operations_.dispatchCommand(surfaceId_, tag, commandName, args);
  }
}

void LoomRenderer::sendAccessibilityEvent(
    Tag tag,
    const std::string& eventType) {
  if (operations_.sendAccessibilityEvent) {
    operations_.sendAccessibilityEvent(surfaceId_, tag, eventType);
  }
}

void LoomRenderer::setIsJSResponder(
    Tag tag,
    bool isJSResponder,
    bool blockNativeResponder) {
  if (operations_.setIsJSResponder) {
    operations_.setIsJSResponder(
        surfaceId_, tag, isJSResponder, blockNativeResponder);
  }
}

InstanceHandle::Shared LoomRenderer::instanceHandle(Tag tag) const {
  const auto* node = find(tag);
  return node == nullptr ? nullptr : node->instanceHandle;
}

SurfaceId LoomRenderer::surfaceId() const {
  return surfaceId_;
}

Tag LoomRenderer::rootTag() const {
  return rootTag_;
}

// Memory slack: a commit that touched a huge tree leaves capacity behind in
// scratch buffers and bucket arrays. Release it once the live size is far
// below it, so a long-lived app that returns to a small screen goes back to a
// small footprint. The thresholds keep steady-state commits allocation free.
void LoomRenderer::releaseSlack() {
  trimScratch(scratchMounted_);
  if (scratchStationary_.bucket_count() > 512 &&
      scratchStationary_.size() * 8 < scratchStationary_.bucket_count()) {
    std::unordered_set<uint64_t>().swap(scratchStationary_);
  }
  const auto shrinkTable = [](auto& table) {
    const auto buckets = table.bucket_count();
    if (buckets > 1024 && table.size() * 8 < buckets) {
      size_t target = 64;
      while (target < table.size() * 2) {
        target <<= 1;
      }
      table.rehash(target);
    }
  };
  shrinkTable(nodes_);
  shrinkTable(rawTextLeaves_);
  trimScratch(layoutNodes_);
  trimScratch(layoutNodeParents_);
}

} // namespace facebook::react
#endif // LOOM_RENDERER
