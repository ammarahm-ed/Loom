/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoomRenderer.h"
#include "LoomRendererInternals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#if LOOM_RENDERER
namespace facebook::react {
namespace {

using loom::findProp;
using loom::numberProp;
using loom::propsFromValue;
using loom::requireArguments;
using loom::stringProp;



} // namespace

/**
 * `pointerEvents` gates both whether a node can be hit itself and whether its
 * children can, exactly as `ConcreteViewShadowNode` decides it. An unset or
 * unrecognized value is `auto`, which allows both.
 */
namespace {
constexpr int kPointerEventsAuto = 0;
constexpr int kPointerEventsBoxNone = 1;
constexpr int kPointerEventsBoxOnly = 2;
constexpr int kPointerEventsNone = 3;

int pointerEventsOf(const folly::dynamic& props) {
  const auto* value = findProp(props, "pointerEvents");
  if (value == nullptr || !value->isString()) {
    return kPointerEventsAuto;
  }
  const auto& name = value->getString();
  if (name == "box-none") {
    return kPointerEventsBoxNone;
  }
  if (name == "box-only") {
    return kPointerEventsBoxOnly;
  }
  if (name == "none") {
    return kPointerEventsNone;
  }
  return kPointerEventsAuto;
}

struct DirectScrollOffset {
  float x{0};
  float y{0};
};

DirectScrollOffset scrollOffsetOf(const folly::dynamic& props) {
  DirectScrollOffset offset;
  if (const auto* value = findProp(props, "scrollLeft");
      value != nullptr && value->isNumber()) {
    offset.x = static_cast<float>(value->asDouble());
  }
  if (const auto* value = findProp(props, "scrollTop");
      value != nullptr && value->isNumber()) {
    offset.y = static_cast<float>(value->asDouble());
  }
  if (const auto* value = findProp(props, "contentOffset");
      value != nullptr && value->isObject()) {
    if (const auto* x = value->get_ptr("x"); x != nullptr && x->isNumber()) {
      offset.x = static_cast<float>(x->asDouble());
    }
    if (const auto* y = value->get_ptr("y"); y != nullptr && y->isNumber()) {
      offset.y = static_cast<float>(y->asDouble());
    }
  }
  return offset;
}

DirectScrollOffset liveScrollOffsetOf(
    const LoomRenderer::NativeOperations& operations,
    SurfaceId surfaceId,
    Tag tag,
    const folly::dynamic& props) {
  if (operations.getScrollOffset) {
    const auto offset = operations.getScrollOffset(surfaceId, tag);
    return {offset.first, offset.second};
  }
  return scrollOffsetOf(props);
}

struct DirectTransform {
  float a{1};
  float b{0};
  float c{0};
  float d{1};
  float tx{0};
  float ty{0};

  std::pair<float, float> apply(float x, float y) const {
    return {a * x + c * y + tx, b * x + d * y + ty};
  }

  std::pair<float, float> inverse(float x, float y) const {
    const auto determinant = a * d - b * c;
    if (std::abs(determinant) < 0.000001f) {
      return {x, y};
    }
    const auto localX = x - tx;
    const auto localY = y - ty;
    return {
        (d * localX - c * localY) / determinant,
        (-b * localX + a * localY) / determinant};
  }
};

DirectTransform multiplyTransform(
    const DirectTransform& left,
    const DirectTransform& right) {
  return DirectTransform{
      left.a * right.a + left.c * right.b,
      left.b * right.a + left.d * right.b,
      left.a * right.c + left.c * right.d,
      left.b * right.c + left.d * right.d,
      left.a * right.tx + left.c * right.ty + left.tx,
      left.b * right.tx + left.d * right.ty + left.ty};
}

float angleRadians(const folly::dynamic& value) {
  if (value.isNumber()) {
    return static_cast<float>(value.asDouble());
  }
  if (!value.isString()) {
    return 0;
  }
  const auto string = value.getString();
  try {
    const auto angle = std::stof(string);
    const bool degrees =
        string.size() >= 3 && string.compare(string.size() - 3, 3, "deg") == 0;
    return degrees ? angle * static_cast<float>(3.14159265358979323846 / 180.0)
                   : angle;
  } catch (const std::exception&) {
    return 0;
  }
}

DirectTransform transformOf(const folly::dynamic& props) {
  const auto* transformProp = findProp(props, "transform");
  if (transformProp == nullptr || !transformProp->isArray()) {
    return {};
  }
  DirectTransform result;
  for (const auto& operation : *transformProp) {
    if (!operation.isObject()) {
      continue;
    }
    DirectTransform next;
    if (const auto* value = operation.get_ptr("translateX");
        value != nullptr && value->isNumber()) {
      next.tx = static_cast<float>(value->asDouble());
    }
    if (const auto* value = operation.get_ptr("translateY");
        value != nullptr && value->isNumber()) {
      next.ty = static_cast<float>(value->asDouble());
    }
    if (const auto* value = operation.get_ptr("scale");
        value != nullptr && value->isNumber()) {
      next.a = next.d = static_cast<float>(value->asDouble());
    }
    if (const auto* value = operation.get_ptr("scaleX");
        value != nullptr && value->isNumber()) {
      next.a = static_cast<float>(value->asDouble());
    }
    if (const auto* value = operation.get_ptr("scaleY");
        value != nullptr && value->isNumber()) {
      next.d = static_cast<float>(value->asDouble());
    }
    if (const auto* value = operation.get_ptr("rotate"); value != nullptr) {
      const auto angle = angleRadians(*value);
      next.a = next.d = std::cos(angle);
      next.b = std::sin(angle);
      next.c = -next.b;
    }
    if (const auto* value = operation.get_ptr("rotateZ"); value != nullptr) {
      const auto angle = angleRadians(*value);
      next.a = next.d = std::cos(angle);
      next.b = std::sin(angle);
      next.c = -next.b;
    }
    if (const auto* value = operation.get_ptr("skewX"); value != nullptr) {
      next.c = std::tan(angleRadians(*value));
    }
    if (const auto* value = operation.get_ptr("skewY"); value != nullptr) {
      next.b = std::tan(angleRadians(*value));
    }
    if (const auto* value = operation.get_ptr("matrix");
        value != nullptr && value->isArray()) {
      if (value->size() == 6) {
        next.a = static_cast<float>((*value)[0].asDouble());
        next.b = static_cast<float>((*value)[1].asDouble());
        next.c = static_cast<float>((*value)[2].asDouble());
        next.d = static_cast<float>((*value)[3].asDouble());
        next.tx = static_cast<float>((*value)[4].asDouble());
        next.ty = static_cast<float>((*value)[5].asDouble());
      } else if (value->size() >= 16) {
        // React Native supplies a 4x4 column-major matrix. The direct
        // renderer is 2D today, so preserve its affine projection.
        next.a = static_cast<float>((*value)[0].asDouble());
        next.b = static_cast<float>((*value)[1].asDouble());
        next.c = static_cast<float>((*value)[4].asDouble());
        next.d = static_cast<float>((*value)[5].asDouble());
        next.tx = static_cast<float>((*value)[12].asDouble());
        next.ty = static_cast<float>((*value)[13].asDouble());
      }
    }
    result = multiplyTransform(result, next);
  }
  return result;
}

bool hasUnsupportedTransform(const folly::dynamic& props) {
  const auto* transformProp = findProp(props, "transform");
  if (transformProp == nullptr || !transformProp->isArray()) {
    return false;
  }
  for (const auto& operation : *transformProp) {
    if (!operation.isObject()) {
      continue;
    }
    for (const auto* name :
         {"perspective", "rotateX", "rotateY", "translateZ", "scaleZ"}) {
      if (operation.get_ptr(name) != nullptr) {
        return true;
      }
    }
    const auto* matrix = operation.get_ptr("matrix");
    if (matrix != nullptr && matrix->isArray() && matrix->size() >= 16) {
      // The Loom renderer supports the affine 2D projection only. Any
      // non-affine/perspective entries require the platform transform model.
      for (const auto index : {3, 7, 11, 14, 15}) {
        if ((*matrix)[index].isNumber() &&
            ((index == 15 && (*matrix)[index].asDouble() != 1.0) ||
             (index != 15 && (*matrix)[index].asDouble() != 0.0))) {
          return true;
        }
      }
    }
  }
  return false;
}

float transformOriginPart(
    const folly::dynamic& value,
    float size,
    bool horizontal) {
  if (value.isNumber()) {
    return static_cast<float>(value.asDouble());
  }
  if (!value.isString()) {
    return size * 0.5f;
  }
  const auto text = value.getString();
  if (text == "center") {
    return size * 0.5f;
  }
  if (text == "left" || text == "top") {
    return 0;
  }
  if (text == "right" || text == "bottom") {
    return size;
  }
  try {
    const auto percent = text.size() > 1 && text.back() == '%';
    const auto number = std::stof(text);
    return percent ? size * number / 100.0f : number;
  } catch (const std::exception&) {
    return horizontal ? size * 0.5f : size * 0.5f;
  }
}

DirectTransform
transformAroundOrigin(const folly::dynamic& props, float width, float height) {
  const auto* origin = findProp(props, "transformOrigin");
  if (origin == nullptr || !origin->isArray() || origin->empty()) {
    return transformOf(props);
  }
  const auto x = transformOriginPart((*origin)[0], width, true);
  const auto y = origin->size() > 1
      ? transformOriginPart((*origin)[1], height, false)
      : height * 0.5f;
  const auto toOrigin = DirectTransform{1, 0, 0, 1, x, y};
  const auto fromOrigin = DirectTransform{1, 0, 0, 1, -x, -y};
  return multiplyTransform(
      toOrigin, multiplyTransform(transformOf(props), fromOrigin));
}

float hitSlopValue(const folly::dynamic& props, const char* key) {
  const auto* hitSlop = findProp(props, "hitSlop");
  if (hitSlop == nullptr) {
    return 0;
  }
  if (hitSlop->isNumber()) {
    return static_cast<float>(hitSlop->asDouble());
  }
  if (hitSlop->isObject()) {
    const auto* value = hitSlop->get_ptr(key);
    return value != nullptr && value->isNumber()
        ? static_cast<float>(value->asDouble())
        : 0;
  }
  return 0;
}

int zIndexOf(const folly::dynamic& props) {
  const auto* value = findProp(props, "zIndex");
  return value != nullptr && value->isNumber() ? value->asInt() : 0;
}
} // namespace

std::pair<float, float> LoomRenderer::scrollOffset(Tag tag) const {
  const auto* node = find(tag);
  if (node == nullptr) {
    return {0, 0};
  }
  const auto offset =
      liveScrollOffsetOf(operations_, surfaceId_, tag, node->props);
  return {offset.x, offset.y};
}

std::pair<float, float> LoomRenderer::scrollContentSize(Tag tag) const {
  const auto* node = find(tag);
  if (node == nullptr || node->yogaNode == nullptr) {
    return {0, 0};
  }
  const auto border = borderWidths(tag);
  const float ownWidth = std::max(
      0.0f,
      YGNodeLayoutGetWidth(node->yogaNode) -
          static_cast<float>(border[0] + border[2]));
  const float ownHeight = std::max(
      0.0f,
      YGNodeLayoutGetHeight(node->yogaNode) -
          static_cast<float>(border[1] + border[3]));
  float minX = 0;
  float minY = 0;
  float maxX = ownWidth;
  float maxY = ownHeight;
  std::function<void(Tag)> visit = [&](Tag parentTag) {
    for (const auto childTag : childrenOf(parentTag)) {
      Metrics childMetrics;
      if (metricsRelativeTo(childTag, tag, childMetrics, false, false)) {
        minX = std::min(minX, childMetrics.x);
        minY = std::min(minY, childMetrics.y);
        maxX = std::max(maxX, childMetrics.x + childMetrics.width);
        maxY = std::max(maxY, childMetrics.y + childMetrics.height);
      }
      visit(childTag);
    }
  };
  visit(tag);
  return {std::max(ownWidth, maxX - minX), std::max(ownHeight, maxY - minY)};
}

Tag LoomRenderer::findNodeAtPoint(Tag tag, float x, float y) const {
  const auto* node = find(tag);
  // A view-less node has no frame to be hit in; its text is part of the
  // paragraph that owns it.
  if (node == nullptr || node->yogaNode == nullptr || node->displayNone) {
    return 0;
  }

  const auto pointerEvents = pointerEventsOf(node->props);
  if (pointerEvents == kPointerEventsNone) {
    return 0;
  }
  if (hasUnsupportedTransform(node->props)) {
    if (operations_.reportUnsupportedTransform) {
      operations_.reportUnsupportedTransform(surfaceId_, node->tag);
    }
    return 0;
  }
  const bool canBeTarget = (pointerEvents == kPointerEventsAuto ||
                            pointerEvents == kPointerEventsBoxOnly) &&
      !node->hostOwned;
  const bool canChildrenBeTarget = pointerEvents == kPointerEventsAuto ||
      pointerEvents == kPointerEventsBoxNone;

  // Convert the point into this node's local coordinate space before testing
  // bounds or descending. This handles translation, scale, and rotation using
  // the same matrix that measurement uses.
  const auto localPoint = transformAroundOrigin(
                              node->props,
                              YGNodeLayoutGetWidth(node->yogaNode),
                              YGNodeLayoutGetHeight(node->yogaNode))
                              .inverse(x, y);
  x = localPoint.first;
  y = localPoint.second;

  const auto width = YGNodeLayoutGetWidth(node->yogaNode);
  const auto height = YGNodeLayoutGetHeight(node->yogaNode);
  const auto leftSlop = hitSlopValue(node->props, "left");
  const auto rightSlop = hitSlopValue(node->props, "right");
  const auto topSlop = hitSlopValue(node->props, "top");
  const auto bottomSlop = hitSlopValue(node->props, "bottom");
  const bool inside = x >= -leftSlop && y >= -topSlop &&
      x < width + rightSlop && y < height + bottomSlop;
  const bool insideBounds = x >= 0 && y >= 0 && x < width && y < height;

  const auto& overflow = stringProp(node->props, "overflow");
  if ((overflow == "hidden" || overflow == "scroll") && !insideBounds) {
    return 0;
  }

  if (!canChildrenBeTarget) {
    // `box-only` never delegates to descendants, even when overflow is
    // visible. It may target itself only inside its own hit rectangle.
    return inside && canBeTarget ? node->tag : 0;
  }

  // Back to front: higher zIndex wins, then later sibling order wins.
  std::vector<Tag> hitChildren = node->children;
  std::stable_sort(
      hitChildren.begin(), hitChildren.end(), [this](Tag left, Tag right) {
        const auto* leftNode = find(left);
        const auto* rightNode = find(right);
        return leftNode != nullptr && rightNode != nullptr &&
            zIndexOf(leftNode->props) < zIndexOf(rightNode->props);
      });
  const auto scroll =
      liveScrollOffsetOf(operations_, surfaceId_, node->tag, node->props);
  for (auto it = hitChildren.rbegin(); it != hitChildren.rend(); ++it) {
    const auto* child = find(*it);
    if (child == nullptr || child->yogaNode == nullptr) {
      continue;
    }
    const auto hit = findNodeAtPoint(
        child->tag,
        x - YGNodeLayoutGetLeft(child->yogaNode) + scroll.x,
        y - YGNodeLayoutGetTop(child->yogaNode) + scroll.y);
    if (hit != 0) {
      return hit;
    }
  }

  return inside && canBeTarget ? node->tag : 0;
}

unsigned LoomRenderer::compareDocumentPosition(Tag tag, Tag otherTag)
    const {
  constexpr unsigned disconnected = 0x01;
  constexpr unsigned preceding = 0x02;
  constexpr unsigned following = 0x04;
  constexpr unsigned contains = 0x08;
  constexpr unsigned containedBy = 0x10;

  if (!exists(tag) || !exists(otherTag)) {
    return disconnected;
  }
  if (tag == otherTag) {
    return 0;
  }
  if (!isConnected(tag) || !isConnected(otherTag)) {
    return disconnected;
  }
  if (tag == rootTag_) {
    // `tag` is the surface root, so everything else in it is contained by it.
    return containedBy | following;
  }
  if (otherTag == rootTag_) {
    return contains | preceding;
  }

  // Raw-text leaves have identity and a parent edge but no Yoga Node. Build
  // paths through the shared parent/child graph so DOM ordering works for
  // both ordinary nodes and paragraph-owned raw text.
  auto ancestryOf = [this](Tag start) {
    std::vector<size_t> chain;
    auto current = start;
    while (current != rootTag_) {
      const auto parentTag = parentOfAny(current);
      const auto* parent = find(parentTag);
      if (parent == nullptr) {
        break;
      }
      const auto position = std::find(
          parent->children.begin(), parent->children.end(), current);
      chain.push_back(
          position == parent->children.end()
              ? parent->children.size()
              : static_cast<size_t>(position - parent->children.begin()));
      current = parentTag;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
  };

  const auto ancestors = ancestryOf(tag);
  const auto otherAncestors = ancestryOf(otherTag);
  size_t index = 0;
  while (index < ancestors.size() && index < otherAncestors.size() &&
         ancestors[index] == otherAncestors[index]) {
    index += 1;
  }

  if (index == ancestors.size()) {
    return containedBy | following;
  }
  if (index == otherAncestors.size()) {
    return contains | preceding;
  }
  return ancestors[index] > otherAncestors[index] ? preceding : following;
}

bool LoomRenderer::metricsRelativeTo(
    Tag tag,
    Tag ancestorTag,
    Metrics& out,
    bool includeTransform,
    bool includeScroll) const {
  const auto* node = find(tag);
  // A view-less node (a span, a raw run, a paragraph's non-text child) has no
  // frame of its own, exactly as it has no layout metrics in the shadow tree.
  if (node == nullptr || node->yogaNode == nullptr) {
    return false;
  }

  const auto ownWidth = YGNodeLayoutGetWidth(node->yogaNode);
  const auto ownHeight = YGNodeLayoutGetHeight(node->yogaNode);
  DirectTransform accumulated;

  // Build one local-to-ancestor matrix. In particular, zero means “surface
  // root”, not “stop after the first parent”. The old implementation did the
  // latter, which made nested measureInWindow/bounding-rect calls wrong.
  for (const auto* current = node; current != nullptr;) {
    if (ancestorTag != 0 && current->tag == ancestorTag) {
      break;
    }
    if (current->yogaNode != nullptr) {
      const auto width = YGNodeLayoutGetWidth(current->yogaNode);
      const auto height = YGNodeLayoutGetHeight(current->yogaNode);
      DirectTransform local{
          1,
          0,
          0,
          1,
          YGNodeLayoutGetLeft(current->yogaNode),
          YGNodeLayoutGetTop(current->yogaNode)};
      if (includeTransform && hasUnsupportedTransform(current->props)) {
        if (operations_.reportUnsupportedTransform) {
          operations_.reportUnsupportedTransform(surfaceId_, current->tag);
        }
        return false;
      }
      if (includeTransform) {
        local = multiplyTransform(
            local, transformAroundOrigin(current->props, width, height));
      }
      accumulated = multiplyTransform(local, accumulated);
    }

    if (current->parent == 0) {
      if (ancestorTag != 0) {
        return false;
      }
      break;
    }
    const auto* parent = find(current->parent);
    if (parent == nullptr) {
      return false;
    }
    if (includeScroll) {
      const auto scroll = liveScrollOffsetOf(
          operations_, surfaceId_, parent->tag, parent->props);
#if LOOM_PROFILE
      std::fprintf(
          stderr,
          "LoomScrollMetric target=%d node=%d parent=%d liveOffsetDip=%.3f,%.3f\n",
          static_cast<int>(tag),
          static_cast<int>(current->tag),
          static_cast<int>(parent->tag),
          scroll.x,
          scroll.y);
#endif
      accumulated = multiplyTransform(
          DirectTransform{1, 0, 0, 1, -scroll.x, -scroll.y}, accumulated);
    }
    current = parent;
  }

  const auto p0 = accumulated.apply(0, 0);
  const auto p1 = accumulated.apply(ownWidth, 0);
  const auto p2 = accumulated.apply(0, ownHeight);
  const auto p3 = accumulated.apply(ownWidth, ownHeight);
  out.x = std::min({p0.first, p1.first, p2.first, p3.first});
  out.y = std::min({p0.second, p1.second, p2.second, p3.second});
  out.width = std::max({p0.first, p1.first, p2.first, p3.first}) - out.x;
  out.height = std::max({p0.second, p1.second, p2.second, p3.second}) - out.y;
  return true;
}

std::optional<std::pair<float, float>> LoomRenderer::viewLocationInSurface(
    Tag tag) const {
  if (!operations_.getViewLocationInSurface) {
    return std::nullopt;
  }
  return operations_.getViewLocationInSurface(surfaceId_, tag);
}


} // namespace facebook::react
#endif // LOOM_RENDERER
