/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomRenderer.h"

#include <limits>
#include <utility>

#include <glog/logging.h>

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
  syncCompatNode(node);
  if (const auto compatYoga =
          node.compat != nullptr ? node.compat->yogaNode() : nullptr;
      compatYoga != nullptr) {
    // The component's own ShadowNode resolved this style: React Native's own
    // props parsing plus whatever its descriptor's `adopt` changed (safe-area
    // insets as padding, for instance). Yoga only re-lays the node out when
    // the copy actually differs.
    YGNodeCopyStyle(node.yogaNode, compatYoga);
    node.styleDirty = false;
  } else if (node.styleDirty) {
    if (shapeValid(node) && !node.hostOwned) {
      // [LoomPerf item 2] Copy the style of a configured template instead of
      // setting it key by key. The template is built once per shape by the
      // same `applyYogaProps`, so the result is identical.
      const auto& shape = *node.shape;
      if (shape.yogaTemplate == nullptr) {
        shape.yogaTemplate = YGNodeNewWithConfig(yogaConfig_);
        const auto live = node.yogaNode;
        node.yogaNode = shape.yogaTemplate;
        applyYogaProps(node);
        node.yogaNode = live;
      }
      YGNodeCopyStyle(node.yogaNode, shape.yogaTemplate);
      node.styleDirty = false;
    } else {
      applyYogaProps(node);
    }
  }
  // Unset direction inherits the owner direction supplied to calculateLayout;
  // an explicit React direction is the only per-node override.
  YGNodeStyleSetDirection(node.yogaNode, node.direction);
  // A paragraph measures its own content, and so does a compat leaf whose
  // ShadowNode is measurable (Switch, ProgressBar, ...); nothing else has a
  // measure function.
  YGNodeSetMeasureFunc(
      node.yogaNode,
      node.textRoot ? &LoomRenderer::measureText
          : (node.compat != nullptr && node.children.empty() &&
             node.compat->measurable())
          ? &LoomRenderer::measureCompat
          : nullptr);
}

void LoomRenderer::syncCompatNode(Node& node) {
  if (!node.compatChecked) {
    node.compatChecked = true;
    node.compatDescriptor = compat_ != nullptr && !node.hostOwned
        ? compat_->descriptorFor(node.componentName)
        : nullptr;
  }
  if (node.compatDescriptor == nullptr) {
    return;
  }
  try {
    // Props the descriptor parsed from the JSI value on the JS thread, when
    // the binding provided them for this props revision; otherwise the merged
    // folly props are parsed here.
    auto parsed = currentCompatProps(node);
    if (node.compat == nullptr) {
      node.compat = parsed != nullptr
          ? std::make_unique<LoomCompatNode>(
                compat_,
                *node.compatDescriptor,
                node.tag,
                surfaceId_,
                node.instanceHandle,
                std::move(parsed))
          : std::make_unique<LoomCompatNode>(
                compat_,
                *node.compatDescriptor,
                node.tag,
                surfaceId_,
                node.instanceHandle,
                node.props);
      node.compat->propsGeneration = node.propsGeneration;
    } else if (node.compat->propsGeneration != node.propsGeneration) {
      if (parsed != nullptr) {
        node.compat->updateProps(std::move(parsed));
      } else {
        node.compat->updateProps(node.props);
      }
      node.compat->propsGeneration = node.propsGeneration;
    }
  } catch (const std::exception& error) {
    // A descriptor that cannot take these props leaves the node on Loom's
    // generic path rather than failing the surface.
    LOG(ERROR) << "Loom compat: " << node.componentName << " tag " << node.tag
               << ": " << error.what();
    node.compat.reset();
    node.compatDescriptor = nullptr;
    return;
  }
  if (node.compat->stateDirty && node.compat->state() != nullptr) {
    node.needsState = true;
    stateUpdates_.push_back(node.tag);
  }
}

YGSize LoomRenderer::measureCompat(
    YGNodeConstRef yogaNode,
    float width,
    YGMeasureMode widthMode,
    float height,
    YGMeasureMode heightMode) {
  const auto* node = static_cast<const Node*>(YGNodeGetContext(yogaNode));
  if (node == nullptr || node->compat == nullptr) {
    return YGSize{0, 0};
  }
  // The constraint mapping of YogaLayoutableShadowNode's measure connector.
  constexpr auto kInfinity = std::numeric_limits<Float>::infinity();
  LayoutConstraints constraints;
  constraints.minimumSize = Size{0, 0};
  constraints.maximumSize = Size{kInfinity, kInfinity};
  if (widthMode == YGMeasureModeExactly) {
    constraints.minimumSize.width = width;
    constraints.maximumSize.width = width;
  } else if (widthMode == YGMeasureModeAtMost) {
    constraints.maximumSize.width = width;
  }
  if (heightMode == YGMeasureModeExactly) {
    constraints.minimumSize.height = height;
    constraints.maximumSize.height = height;
  } else if (heightMode == YGMeasureModeAtMost) {
    constraints.maximumSize.height = height;
  }
  LayoutContext context;
  context.pointScaleFactor =
      node->owner != nullptr ? node->owner->pointScaleFactor_ : 1.0f;
  const auto size = node->compat->measure(context, constraints);
  return YGSize{static_cast<float>(size.width), static_cast<float>(size.height)};
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
