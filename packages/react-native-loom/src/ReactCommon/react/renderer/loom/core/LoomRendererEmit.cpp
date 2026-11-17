/* Copyright (c) Meta Platforms, Inc. and affiliates. */

// LoomRenderer: emitting mount operations (create, update, insert, remove, layout,
// delete) and layout events.

#include "LoomRendererShared.h"

#if LOOM_RENDERER
namespace facebook::react {

void LoomRenderer::emitCreate(Node& node) {
  if (node.hostOwned) {
    // The surface root view belongs to the host; this renderer only ever adds
    // children to it.
    return;
  }
  // Virtual text still gets a create so React-facing tag bookkeeping remains
  // available. Raw text leaves are stored separately and never reach here.
  // View-less tags are not inserted, which keeps text spans out of the native
  // view hierarchy.
  node.propsDelta = nullptr;
  pendingOperations_.append(DirectCreateOperation{
      node.tag,
      !node.virtualNode && !node.layoutOnlyNode,
      node.componentName,
      node.layoutOnlyNode ? LoomSharedProps() : node.props,
      currentCompatProps(node)});
  pendingOperations_.requiresTagReplacement =
      pendingOperations_.requiresTagReplacement ||
      node.replacesHostTagThisCommit;
  node.hostCreated = true;
  node.hostLayoutable = !node.virtualNode && !node.layoutOnlyNode;
  node.createdThisCommit = true;
  node.replacesHostTagThisCommit = false;
  node.propsDirty = false;
}

void LoomRenderer::emitUpdateProps(Node& node) {
  if (node.hostOwned || node.layoutOnlyNode || !node.propsDirty) {
    return;
  }
  if (!node.roleFlagsValid) {
    refreshRoleFlags(node);
  }
  if (node.roleSendsPropDiffs && node.propsDelta.isObject() &&
      !node.propsDelta.empty()) {
    pendingOperations_.append(DirectUpdatePropsOperation{
        node.tag, std::move(node.propsDelta), currentCompatProps(node)});
    node.propsDelta = nullptr;
    node.propsDirty = false;
    return;
  }
  pendingOperations_.append(
      DirectUpdatePropsOperation{node.tag, node.props, currentCompatProps(node)});
  node.propsDelta = nullptr;
  node.propsDirty = false;
}

std::shared_ptr<const Props> LoomRenderer::currentCompatProps(const Node& node) {
  return node.compatProps != nullptr &&
          node.compatPropsGeneration == node.propsGeneration
      ? node.compatProps
      : nullptr;
}

void LoomRenderer::setCompatProps(Tag tag, std::shared_ptr<const Props> props) {
  auto* node = find(tag);
  if (node == nullptr) {
    return;
  }
  node->compatProps = std::move(props);
  node->compatPropsGeneration = node->propsGeneration;
}

void LoomRenderer::emitRemovals(
    Node& node,
    const std::unordered_set<uint64_t>& stationaryChildren) {
  auto& mounted = node.mountedChildren;
  for (size_t index = mounted.size(); index-- > 0;) {
    if (stationaryChildren.count(childPairKey(node.tag, mounted[index])) != 0) {
      continue;
    }
    pendingOperations_.append(
        DirectRemoveOperation{node.tag, mounted[index], index});
  }
}

void LoomRenderer::emitInsertions(
    Node& node,
    const Tag* next,
    size_t nextCount,
    const std::unordered_set<uint64_t>& stationaryChildren) {
  const bool carryLayout =
      LoomPlatformTraits::current().insertsCarryInitialLayout;
  for (size_t index = 0; index < nextCount; ++index) {
    if (stationaryChildren.count(childPairKey(node.tag, next[index])) != 0) {
      continue;
    }
    if (carryLayout) {
      if (const auto* child = find(next[index]);
          child != nullptr && child->createdThisCommit) {
        initialInsertIndexes_[next[index]] =
            pendingOperations_.operations.size();
      }
    }
    pendingOperations_.append(
        DirectInsertOperation{node.tag, next[index], index});
  }
  if (nextCount == 0) {
    node.mountedChildren.clear();
  } else {
    node.mountedChildren.assign(next, next + nextCount);
  }
}

void LoomRenderer::emitLayout(
    Node& node,
    const LayoutParent& parent,
    const std::array<float, 4>& overflowInsets) {
  if (node.hostOwned || node.yogaNode == nullptr) {
    return;
  }
  const auto left = YGNodeLayoutGetLeft(node.yogaNode);
  const auto top = YGNodeLayoutGetTop(node.yogaNode);
  const auto width = YGNodeLayoutGetWidth(node.yogaNode);
  const auto height = YGNodeLayoutGetHeight(node.yogaNode);
  const auto displayType = node.displayNone ? kDisplayNone : kDisplayFlex;
  const auto direction =
      YGNodeLayoutGetDirection(node.yogaNode) == YGDirectionRTL
      ? kLayoutDirectionRTL
      : kLayoutDirectionLTR;

  float flattenedOffsetX = parent.offsetX;
  float flattenedOffsetY = parent.offsetY;
  auto ancestorTag = parentOfAny(node.tag);
  while (ancestorTag != 0 && ancestorTag != parent.tag) {
    const auto* ancestor = find(ancestorTag);
    if (ancestor == nullptr) {
      break;
    }
    if (ancestor->layoutOnlyNode && ancestor->yogaNode != nullptr) {
      flattenedOffsetX += YGNodeLayoutGetLeft(ancestor->yogaNode);
      flattenedOffsetY += YGNodeLayoutGetTop(ancestor->yogaNode);
    }
    ancestorTag = parentOfAny(ancestorTag);
  }
  const auto nativeLeft = flattenedOffsetX + left;
  const auto nativeTop = flattenedOffsetY + top;

  const auto edge = [&](YGEdge yogaEdge) {
    return YGNodeLayoutGetBorder(node.yogaNode, yogaEdge) +
        YGNodeLayoutGetPadding(node.yogaNode, yogaEdge);
  };
  const std::array<float, 4> contentInsets = {
      edge(YGEdgeLeft), edge(YGEdgeTop), edge(YGEdgeRight), edge(YGEdgeBottom)};

  /*
   * The recursive calculation intentionally uses the current Yoga result,
   * rather than a child's previously mounted inset. Parent overflow therefore
   * updates in the same direct batch when a descendant moves or resizes.
   */
  constexpr float epsilon = 0.01f;
  const bool frameChanged = !node.hasMountedLayout ||
      std::abs(left - node.mountedLeft) > epsilon ||
      std::abs(top - node.mountedTop) > epsilon ||
      std::abs(width - node.mountedWidth) > epsilon ||
      std::abs(height - node.mountedHeight) > epsilon ||
      parent.tag != node.mountedParentTag ||
      displayType != node.mountedDisplayType ||
      direction != node.mountedDirection;
  const bool nativePositionChanged =
      std::abs(nativeLeft - node.mountedNativeLeft) > epsilon ||
      std::abs(nativeTop - node.mountedNativeTop) > epsilon;
  const bool insetsChanged = !node.hasMountedInsets ||
      contentInsets != node.mountedContentInsets ||
      overflowInsets != node.mountedOverflowInsets;
  if (node.layoutOnlyNode) {
    if (frameChanged) {
      const auto* wantsLayout = findProp(node.props, "onLayout");
      if (wantsLayout != nullptr && wantsLayout->isBool() &&
          wantsLayout->asBool()) {
        layoutEvents_.push_back(node.tag);
      }
      node.mountedLeft = left;
      node.mountedTop = top;
      node.mountedWidth = width;
      node.mountedHeight = height;
      node.mountedParentTag = parent.tag;
      node.mountedDisplayType = displayType;
      node.mountedDirection = direction;
      node.hasMountedLayout = true;
    }
    return;
  }

  const bool changed = frameChanged || nativePositionChanged;
  if (!changed && !insetsChanged) {
    return;
  }

  // `insertsCarryInitialLayout`: a view created in this batch starts at zero
  // insets, so only non-zero ones are sent, and its first frame rides on its
  // insert.
  const bool initialLayout = node.createdThisCommit &&
      LoomPlatformTraits::current().insertsCarryInitialLayout;
  if (insetsChanged) {
    constexpr std::array<float, 4> kNoInsets{};
    if (!initialLayout || contentInsets != kNoInsets) {
      pendingOperations_.append(
          DirectPaddingOperation{node.tag, contentInsets});
    }
    if (!initialLayout || overflowInsets != kNoInsets) {
      pendingOperations_.append(
          DirectOverflowInsetOperation{node.tag, overflowInsets});
    }
    node.mountedContentInsets = contentInsets;
    node.mountedOverflowInsets = overflowInsets;
    node.hasMountedInsets = true;
  }

  // A host that draws text in the state's content size needs a resize
  // republished even when the text itself is unchanged.
  if (LoomPlatformTraits::current().republishTextStateOnResize &&
      node.textRoot && (changed || insetsChanged) &&
      node.hasTextMeasurementCache) {
    node.needsState = true;
    stateUpdates_.push_back(node.tag);
  }

  if (!changed) {
    return;
  }

  auto insert = initialLayout ? initialInsertIndexes_.find(node.tag)
                              : initialInsertIndexes_.end();
  if (insert != initialInsertIndexes_.end()) {
    auto& carried = std::get<DirectInsertOperation>(
        pendingOperations_.operations[insert->second]);
    carried.hasLayout = true;
    carried.displayType = displayType;
    carried.layoutDirection = direction;
    carried.x = nativeLeft;
    carried.y = nativeTop;
    carried.width = width;
    carried.height = height;
    pendingOperations_.instructionCount += 6;
  } else {
    pendingOperations_.append(DirectLayoutOperation{
        node.tag,
        parent.tag,
        displayType,
        direction,
        nativeLeft,
        nativeTop,
        width,
        height});
  }

  node.mountedLeft = left;
  node.mountedTop = top;
  node.mountedWidth = width;
  node.mountedHeight = height;
  node.mountedParentTag = parent.tag;
  node.mountedNativeLeft = nativeLeft;
  node.mountedNativeTop = nativeTop;
  node.mountedDisplayType = displayType;
  node.mountedDirection = direction;
  node.hasMountedLayout = true;

  // A node only reports a layout when it has one, which is what the `onLayout`
  // prop records on the JS side.
  const auto* wantsLayout = findProp(node.props, "onLayout");
  if (frameChanged && wantsLayout != nullptr && wantsLayout->isBool() &&
      wantsLayout->asBool()) {
    layoutEvents_.push_back(node.tag);
  }
}

void LoomRenderer::emitLayoutEvents() {
  // Sent as a batch after the host has the new geometry, so a handler that
  // measures straight away sees the frame it was told about.
  for (const auto tag : layoutEvents_) {
    if (!isSurfaceActive()) {
      break;
    }
    const auto* node = find(tag);
    if (node == nullptr || node->yogaNode == nullptr) {
      continue;
    }
    if (operations_.dispatchEvent) {
      operations_.dispatchEvent(
          surfaceId_,
          tag,
          // The shadow-tree path normalizes "layout" to "topLayout" on the way
          // out, and that is the name the JS renderer matches on.
          "topLayout",
          folly::dynamic::object(
              "layout",
              folly::dynamic::object(
                  "x",
                  static_cast<double>(YGNodeLayoutGetLeft(node->yogaNode)))(
                  "y", static_cast<double>(YGNodeLayoutGetTop(node->yogaNode)))(
                  "width",
                  static_cast<double>(YGNodeLayoutGetWidth(node->yogaNode)))(
                  "height",
                  static_cast<double>(YGNodeLayoutGetHeight(node->yogaNode)))),
          RawEvent::Category::Unspecified,
          false);
    }
  }
  layoutEvents_.clear();
}

void LoomRenderer::emitDelete(Tag tag) {
  pendingOperations_.append(DirectDeleteOperation{tag});
}

} // namespace facebook::react
#endif // LOOM_RENDERER
