/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomRenderer.h"

#include <utility>

#if LOOM_RENDERER
namespace facebook::react {

void LoomRenderer::syncYogaNode(Node& node) {
  if (node.virtualNode) {
    return;
  }
  if (node.yogaNode == nullptr) {
    node.yogaNode = YGNodeNewWithConfig(yogaConfig_);
    YGNodeSetContext(node.yogaNode, &node);
    node.styleDirty = true;
  }
  if (node.styleDirty) {
    applyYogaProps(node);
  }
  // Unset direction inherits the owner direction supplied to calculateLayout;
  // an explicit React direction is the only per-node override.
  YGNodeStyleSetDirection(node.yogaNode, node.direction);
  // A paragraph measures its own content; nothing else has a measure function.
  YGNodeSetMeasureFunc(
      node.yogaNode, node.textRoot ? &LoomRenderer::measureText : nullptr);
}

void LoomRenderer::syncYogaChildren(Node& node) {
  // Compare in place before materialising the list. A steady-state commit
  // normally finds the Yoga child list already correct, and the old order built
  // `next` first and compared afterwards, so every node with children paid one
  // vector allocation per commit for a result it discarded. This walk is
  // deliberately conservative: it returns early only when the live children
  // match `yogaChildren` element for element and to the same length, which is
  // exactly what the vector comparison below decides. Anything else - including
  // a list that is merely a prefix of the stored one - falls through to the
  // original build and compare, so the decision cannot change.
  bool matches = true;
  auto expected = node.yogaChildren.begin();
  for (const Tag childTag : node.children) {
    const auto* child = find(childTag);
    if (child == nullptr || child->virtualNode) {
      continue;
    }
    if (expected == node.yogaChildren.end() || *expected != childTag) {
      matches = false;
      break;
    }
    ++expected;
  }
  if (matches && expected == node.yogaChildren.end()) {
    return;
  }

  std::vector<Tag> next;
  next.reserve(node.children.size());
  for (const Tag childTag : node.children) {
    auto* child = find(childTag);
    if (child != nullptr && !child->virtualNode) {
      next.push_back(childTag);
    }
  }
  if (next == node.yogaChildren) {
    return;
  }
  YGNodeRemoveAllChildren(node.yogaNode);
  for (const Tag childTag : next) {
    auto* child = find(childTag);
    if (child == nullptr || child->yogaNode == nullptr) {
      continue;
    }
    YGNodeInsertChild(
        node.yogaNode, child->yogaNode, YGNodeGetChildCount(node.yogaNode));
  }
  node.yogaChildren = std::move(next);
}

void LoomRenderer::layoutTree() {
  auto* root = find(rootTag_);
  if (root == nullptr || root->yogaNode == nullptr) {
    return;
  }

  // Yoga infers the root's sizing mode from the root's own style, so the
  // surface constraints are expressed exactly the way the shadow-tree path
  // expresses them: as min/max dimensions on the root, with the maximum also
  // passed as the owner size so percent styles resolve against it.
  if (minWidth_ != laidOutMinWidth_ || maxWidth_ != laidOutMaxWidth_) {
    YGNodeStyleSetMinWidth(root->yogaNode, minWidth_);
    YGNodeStyleSetMaxWidth(
        root->yogaNode, maxWidth_ > 0 ? maxWidth_ : YGUndefined);
    laidOutMinWidth_ = minWidth_;
    laidOutMaxWidth_ = maxWidth_;
  }
  if (minHeight_ != laidOutMinHeight_ || maxHeight_ != laidOutMaxHeight_) {
    YGNodeStyleSetMinHeight(root->yogaNode, minHeight_);
    YGNodeStyleSetMaxHeight(
        root->yogaNode, maxHeight_ > 0 ? maxHeight_ : YGUndefined);
    laidOutMinHeight_ = minHeight_;
    laidOutMaxHeight_ = maxHeight_;
  }

  YGNodeCalculateLayout(
      root->yogaNode,
      maxWidth_ > 0 ? maxWidth_ : YGUndefined,
      maxHeight_ > 0 ? maxHeight_ : YGUndefined,
      isRTL_ ? YGDirectionRTL : YGDirectionLTR);
#if LOOM_PROFILE && defined(__ANDROID__)
  // TEMPORARY DIAGNOSTIC: is the root actually bounded to the viewport?
  __android_log_print(
      ANDROID_LOG_INFO,
      "LoomRootSize",
      "surface=%d rootW=%.1f rootH=%.1f",
      static_cast<int>(surfaceId_),
      YGNodeLayoutGetWidth(root->yogaNode),
      YGNodeLayoutGetHeight(root->yogaNode));
#endif
}

} // namespace facebook::react
#endif // LOOM_RENDERER
