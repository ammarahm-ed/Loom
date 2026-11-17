/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomRenderer.h"
#include "LoomRendererInternals.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

#if LOOM_RENDERER
namespace facebook::react {
namespace {

using loom::findProp;
using loom::numberProp;
using loom::propsFromValue;
using loom::requireArguments;
using loom::stringProp;




} // namespace

LoomRenderer::Node* LoomRenderer::find(Tag tag) {
  auto& entry = findCache_[findCacheSlot(tag)];
  if (entry.node != nullptr && entry.tag == tag) {
    return entry.node;
  }
  const auto iterator = nodes_.find(tag);
  if (iterator == nodes_.end()) {
    return nullptr;
  }
  entry.tag = tag;
  entry.node = iterator->second.get();
  return entry.node;
}

const LoomRenderer::Node* LoomRenderer::find(Tag tag) const {
  const auto iterator = nodes_.find(tag);
  return iterator == nodes_.end() ? nullptr : iterator->second.get();
}

LoomRenderer::RawTextLeaf* LoomRenderer::findRawText(Tag tag) {
  const auto iterator = rawTextLeaves_.find(tag);
  return iterator == rawTextLeaves_.end() ? nullptr : &iterator->second;
}

const LoomRenderer::RawTextLeaf* LoomRenderer::findRawText(
    Tag tag) const {
  const auto iterator = rawTextLeaves_.find(tag);
  return iterator == rawTextLeaves_.end() ? nullptr : &iterator->second;
}

Tag LoomRenderer::parentOfAny(Tag tag) const {
  if (const auto* node = find(tag); node != nullptr) {
    return node->parent;
  }
  if (const auto* leaf = findRawText(tag); leaf != nullptr) {
    return leaf->parent;
  }
  return 0;
}

void LoomRenderer::detachRawTextFromParent(RawTextLeaf& leaf) {
  if (leaf.parent == 0) {
    return;
  }
  const auto oldParent = leaf.parent;
  if (auto* parent = find(oldParent); parent != nullptr) {
    parent->children.erase(
        std::remove(parent->children.begin(), parent->children.end(), leaf.tag),
        parent->children.end());
    parent->childrenDirty = true;
    markNeedsPrepare(*parent);
    markTextDirty(oldParent);
  }
  leaf.parent = 0;
  noteOrphanCandidate(leaf.tag);
}

void LoomRenderer::markNeedsPrepare(Node& node) {
  node.prepareSelf = true;
  // Ancestors carry `prepareDescendant` as a path to the change; stop at the
  // first one that already has it, since everything above it does too.
  for (Tag tag = node.parent; tag != 0;) {
    auto* ancestor = find(tag);
    if (ancestor == nullptr || ancestor->prepareDescendant) {
      break;
    }
    ancestor->prepareDescendant = true;
    tag = ancestor->parent;
  }
}

void LoomRenderer::markNeedsPrepare(Tag tag) {
  if (auto* node = find(tag); node != nullptr) {
    markNeedsPrepare(*node);
  }
}

std::optional<LoomRenderer::FabricNodeInfo> LoomRenderer::fabricNodeInfo(
    Tag tag) const {
  const auto* node = find(tag);
  if (node == nullptr) {
    return std::nullopt;
  }
  FabricNodeInfo info;
  info.componentName = &node->componentName;
  info.props = &node->props.get();
  info.propsGeneration = node->propsGeneration;
  info.identity = node->identity;
  info.parent = node->parent;
  info.layoutOnly = node->layoutOnlyNode;
  info.virtualNode = node->virtualNode;
  info.compat = node->compat.get();
  return info;
}

size_t LoomRenderer::mountedChildCount(Tag tag) const {
  const auto* node = find(tag);
  return node == nullptr ? 0 : node->mountedChildren.size();
}

bool LoomRenderer::exists(Tag tag) const {
  return find(tag) != nullptr || findRawText(tag) != nullptr;
}

std::vector<Tag> LoomRenderer::childrenOf(Tag tag) const {
  const auto* node = find(tag);
  return node == nullptr ? std::vector<Tag>{} : node->children;
}

const std::vector<Tag>* LoomRenderer::childrenOfRef(Tag tag) const {
  const auto* node = find(tag);
  return node == nullptr ? nullptr : &node->children;
}

bool LoomRenderer::isConnected(Tag tag) const {
  if (!exists(tag)) {
    return false;
  }
  TagHashSet visited;
  auto current = tag;
  while (current != 0 && visited.insert(current).second) {
    if (current == rootTag_) {
      return true;
    }
    current = parentOfAny(current);
  }
  return false;
}

Tag LoomRenderer::findByNativeId(const std::string& nativeId) const {
  if (nativeId.empty()) {
    return 0;
  }
  for (const auto& entry : nodes_) {
    const auto* value = findProp(entry.second->props, "nativeID");
    if (value != nullptr && value->isString() &&
        value->getString() == nativeId && isConnected(entry.first)) {
      return entry.first;
    }
  }
  return 0;
}

std::string LoomRenderer::componentName(Tag tag) const {
  const auto* node = find(tag);
  if (node != nullptr) {
    return node->componentName;
  }
  return findRawText(tag) == nullptr
      ? std::string{}
      : LoomPlatformTraits::current().components.rawTextHostName;
}

std::string LoomRenderer::textContent(Tag tag) const {
  if (const auto* leaf = findRawText(tag); leaf != nullptr) {
    return leaf->text;
  }
  const auto* node = find(tag);
  if (node == nullptr) {
    return {};
  }
  std::string result;
  for (const auto child : node->children) {
    result += textContent(child);
  }
  return result;
}

double LoomRenderer::numericProp(Tag tag, const char* name) const {
  const auto* node = find(tag);
  const auto* value = node == nullptr ? nullptr : findProp(node->props, name);
  if (value == nullptr && node != nullptr) {
    if (const auto* style = findProp(node->props, "style");
        style != nullptr && style->isObject()) {
      value = findProp(*style, name);
    }
  }
  return value != nullptr && value->isNumber() ? value->asDouble() : 0;
}

std::array<double, 4> LoomRenderer::borderWidths(Tag tag) const {
  const auto* node = find(tag);
  if (node == nullptr) {
    return {};
  }
  const auto number = [&](const char* name) -> std::optional<double> {
    const auto* value = findProp(node->props, name);
    return value != nullptr && value->isNumber()
        ? std::optional<double>(value->asDouble())
        : std::nullopt;
  };
  const auto all = number("borderWidth");
  const auto horizontal = number("borderHorizontalWidth");
  const auto vertical = number("borderVerticalWidth");
  const auto start = number("borderStartWidth");
  const auto end = number("borderEndWidth");
  const auto top = number("borderTopWidth");
  const auto right = number("borderRightWidth");
  const auto bottom = number("borderBottomWidth");
  const auto left = number("borderLeftWidth");
  const bool rtl = node->direction == YGDirectionRTL || isRTL_;
  const auto resolveHorizontal = [&](const std::optional<double>& physical,
                                     const std::optional<double>& logical) {
    if (physical.has_value())
      return *physical;
    if (logical.has_value())
      return *logical;
    if (horizontal.has_value())
      return *horizontal;
    return all.value_or(0.0);
  };
  return {
      resolveHorizontal(left, rtl ? end : start),
      top.value_or(vertical.value_or(all.value_or(0.0))),
      resolveHorizontal(right, rtl ? start : end),
      bottom.value_or(vertical.value_or(all.value_or(0.0)))};
}

bool LoomRenderer::isPositioned(Tag tag) const {
  const auto* node = find(tag);
  const auto* value =
      node == nullptr ? nullptr : findProp(node->props, "position");
  if (value == nullptr && node != nullptr) {
    if (const auto* style = findProp(node->props, "style");
        style != nullptr && style->isObject()) {
      value = findProp(*style, "position");
    }
  }
  return value != nullptr && value->isString() &&
      value->getString() != "static";
}

bool LoomRenderer::isCommitting() const {
  return insideCommit_;
}

LoomComponentTable::Roles LoomRenderer::componentRoles(Tag tag) const {
  if (const auto* node = find(tag); node != nullptr) {
    return node->componentRoles;
  }
  LoomComponentTable::Roles roles;
  if (findRawText(tag) != nullptr) {
    roles.flags = LoomComponentTable::RawText;
  }
  return roles;
}

bool LoomRenderer::isTextNode(Tag tag) const {
  const auto* node = find(tag);
  return node != nullptr && node->textRoot;
}

Tag LoomRenderer::parentOf(Tag tag) const {
  return parentOfAny(tag);
}

const char* LoomRenderer::internComponentName(
    const std::string& name) const {
  // A text fragment's parent view may outlive its renderer node.
  return internedComponentNames_.insert(name).first->c_str();
}

ShadowView LoomRenderer::shadowView(Tag tag) const {
  auto view = ShadowView{};
  view.tag = tag;
  view.surfaceId = surfaceId_;
  view.componentHandle = static_cast<ComponentHandle>(tag);
  if (const auto* node = find(tag); node != nullptr) {
    view.componentName = internComponentName(node->componentName);
    if (node->yogaNode != nullptr) {
      view.layoutMetrics.frame = Rect{
          .origin = {.x = 0, .y = 0},
          .size = {
              .width = YGNodeLayoutGetWidth(node->yogaNode),
              .height = YGNodeLayoutGetHeight(node->yogaNode)}};
    }
  } else if (findRawText(tag) != nullptr) {
    view.componentName = internComponentName(
        LoomPlatformTraits::current().components.rawTextHostName);
  }
  return view;
}

} // namespace facebook::react
#endif // LOOM_RENDERER
