/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomRenderer.h"

#include <limits>
#include <utility>

#include <glog/logging.h>
#include <yoga/node/Node.h>

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
          node.compatNode() != nullptr ? node.compatNode()->yogaNode() : nullptr;
      compatYoga != nullptr) {
    // The component's own ShadowNode resolved this style: React Native's own
    // props parsing plus whatever its descriptor's `adopt` changed (safe-area
    // insets as padding, for instance). Yoga only re-lays the node out when
    // the copy actually differs.
    YGNodeCopyStyle(node.yogaNode, compatYoga);
    node.styleDirty = false;
    // Fabric dirties a measurable node's Yoga node when a new revision (props,
    // state, or `adopt` calling `dirtyLayout`) invalidates its measurement,
    // so Yoga asks `measureContent` again; a style copy alone keeps Yoga's
    // cached measurement. The component's Yoga node is never laid out, so its
    // dirty flag is only that signal: forward it, then consume it.
    if (YGNodeIsDirty(compatYoga)) {
      if (YGNodeHasMeasureFunc(node.yogaNode)) {
        YGNodeMarkDirty(node.yogaNode);
      }
      const_cast<yoga::Node*>(static_cast<const yoga::Node*>(compatYoga))
          ->setDirty(false);
    }
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
          : (node.compatNode() != nullptr && node.children.empty() &&
             node.compatNode()->measurable())
          ? &LoomRenderer::measureCompat
          : nullptr);
}

void LoomRenderer::syncCompatNode(Node& node) {
  if (!node.compatChecked) {
    node.compatChecked = true;
    const auto* descriptor = compat_ != nullptr && !node.hostOwned
        ? compat_->descriptorFor(node.componentName)
        : nullptr;
    if (descriptor != nullptr) {
      node.ensureCold().compatDescriptor = descriptor;
    } else if (node.cold != nullptr) {
      node.cold->compatDescriptor = nullptr;
    }
  }
  if (node.compatDescriptor() == nullptr) {
    return;
  }
  auto& cold = *node.cold;
  try {
    // Props the descriptor parsed from the JSI value on the JS thread, when
    // the binding provided them for this props revision; otherwise the merged
    // folly props are parsed here.
    auto parsed = currentCompatProps(node);
    if (cold.compat == nullptr) {
      cold.compat = parsed != nullptr
          ? std::make_unique<LoomCompatNode>(
                compat_,
                *cold.compatDescriptor,
                node.tag,
                surfaceId_,
                node.instanceHandle,
                std::move(parsed))
          : std::make_unique<LoomCompatNode>(
                compat_,
                *cold.compatDescriptor,
                node.tag,
                surfaceId_,
                node.instanceHandle,
                node.props);
      cold.compat->propsGeneration = node.propsGeneration;
    } else if (cold.compat->propsGeneration != node.propsGeneration) {
      if (parsed != nullptr) {
        cold.compat->updateProps(std::move(parsed));
      } else {
        cold.compat->updateProps(node.props);
      }
      cold.compat->propsGeneration = node.propsGeneration;
    }
  } catch (const std::exception& error) {
    // A descriptor that cannot take these props leaves the node on Loom's
    // generic path rather than failing the surface.
    LOG(ERROR) << "Loom compat: " << node.componentName << " tag " << node.tag
               << ": " << error.what();
    node.resetCompat();
    return;
  }
  if (cold.compat->stateDirty && cold.compat->state() != nullptr) {
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
  if (node == nullptr || node->compatNode() == nullptr) {
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
      rendererOf(yogaNode) != nullptr ? rendererOf(yogaNode)->pointScaleFactor_
                                      : 1.0f;
  const auto size = node->compatNode()->measure(context, constraints);
  return YGSize{static_cast<float>(size.width), static_cast<float>(size.height)};
}

void LoomRenderer::syncYogaChildren(Node& node) {
  // Yoga's own child list is the record of what is attached: compare the
  // live, Yoga-backed children against it in place, and rebuild it only when
  // they differ. A steady-state commit finds it correct and allocates nothing.
  bool matches = true;
  const auto attached = YGNodeGetChildCount(node.yogaNode);
  size_t expected = 0;
  for (const Tag childTag : node.children) {
    const auto* child = find(childTag);
    if (child == nullptr || child->virtualNode || child->yogaNode == nullptr) {
      continue;
    }
    if (expected == attached ||
        YGNodeGetChild(node.yogaNode, expected) != child->yogaNode) {
      matches = false;
      break;
    }
    ++expected;
  }
  if (matches && expected == attached) {
    return;
  }
  YGNodeRemoveAllChildren(node.yogaNode);
  for (const Tag childTag : node.children) {
    auto* child = find(childTag);
    if (child == nullptr || child->virtualNode || child->yogaNode == nullptr) {
      continue;
    }
    YGNodeInsertChild(
        node.yogaNode, child->yogaNode, YGNodeGetChildCount(node.yogaNode));
  }
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
#if LOOM_PROFILE
  if (LoomWorkerProfile::enabled()) {
    // TEMPORARY DIAGNOSTIC: is the root actually bounded to the viewport?
    loomLog(
        LoomLogLevel::Profile,
        "LoomRootSize",
        "surface=%d rootW=%.1f rootH=%.1f",
        static_cast<int>(surfaceId_),
        YGNodeLayoutGetWidth(root->yogaNode),
        YGNodeLayoutGetHeight(root->yogaNode));
  }
#endif
}

} // namespace facebook::react
#endif // LOOM_RENDERER
