/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "UIThreadRenderer.h"
#include "UIThreadRendererInternals.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

#if UI_THREAD_RENDERER
namespace facebook::react {
namespace {

using uiThreadRenderer::isParagraphName;
using uiThreadRenderer::isRawTextName;
using uiThreadRenderer::isVirtualTextName;
using uiThreadRenderer::findProp;
using uiThreadRenderer::numberProp;
using uiThreadRenderer::propsFromValue;
using uiThreadRenderer::requireArguments;
using uiThreadRenderer::stringProp;




} // namespace

UIThreadRenderer::Node* UIThreadRenderer::find(Tag tag) {
  const auto iterator = nodes_.find(tag);
  return iterator == nodes_.end() ? nullptr : iterator->second.get();
}

const UIThreadRenderer::Node* UIThreadRenderer::find(Tag tag) const {
  const auto iterator = nodes_.find(tag);
  return iterator == nodes_.end() ? nullptr : iterator->second.get();
}

UIThreadRenderer::RawTextLeaf* UIThreadRenderer::findRawText(Tag tag) {
  const auto iterator = rawTextLeaves_.find(tag);
  return iterator == rawTextLeaves_.end() ? nullptr : &iterator->second;
}

const UIThreadRenderer::RawTextLeaf* UIThreadRenderer::findRawText(
    Tag tag) const {
  const auto iterator = rawTextLeaves_.find(tag);
  return iterator == rawTextLeaves_.end() ? nullptr : &iterator->second;
}

Tag UIThreadRenderer::parentOfAny(Tag tag) const {
  if (const auto* node = find(tag); node != nullptr) {
    return node->parent;
  }
  if (const auto* leaf = findRawText(tag); leaf != nullptr) {
    return leaf->parent;
  }
  return 0;
}

void UIThreadRenderer::detachRawTextFromParent(RawTextLeaf& leaf) {
  if (leaf.parent == 0) {
    return;
  }
  const auto oldParent = leaf.parent;
  if (auto* parent = find(oldParent); parent != nullptr) {
    parent->children.erase(
        std::remove(parent->children.begin(), parent->children.end(), leaf.tag),
        parent->children.end());
    parent->childrenDirty = true;
    markTextDirty(oldParent);
  }
  leaf.parent = 0;
}

bool UIThreadRenderer::exists(Tag tag) const {
  return find(tag) != nullptr || findRawText(tag) != nullptr;
}

std::vector<Tag> UIThreadRenderer::childrenOf(Tag tag) const {
  const auto* node = find(tag);
  return node == nullptr ? std::vector<Tag>{} : node->children;
}

const std::vector<Tag>* UIThreadRenderer::childrenOfRef(Tag tag) const {
  const auto* node = find(tag);
  return node == nullptr ? nullptr : &node->children;
}

bool UIThreadRenderer::isConnected(Tag tag) const {
  if (!exists(tag)) {
    return false;
  }
  std::unordered_set<Tag> visited;
  auto current = tag;
  while (current != 0 && visited.insert(current).second) {
    if (current == rootTag_) {
      return true;
    }
    current = parentOfAny(current);
  }
  return false;
}

Tag UIThreadRenderer::findByNativeId(const std::string& nativeId) const {
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

std::string UIThreadRenderer::componentName(Tag tag) const {
  const auto* node = find(tag);
  if (node != nullptr) {
    return node->componentName;
  }
  return findRawText(tag) == nullptr ? std::string{} : "RCTRawText";
}

const std::string* UIThreadRenderer::componentNameRef(Tag tag) const {
  if (const auto* node = find(tag); node != nullptr) {
    return &node->componentName;
  }
  if (findRawText(tag) != nullptr) {
    // Matches what `componentName` reports for a raw-text leaf.
    static const auto rawTextComponentName = std::string{"RCTRawText"};
    return &rawTextComponentName;
  }
  return nullptr;
}

std::string UIThreadRenderer::textContent(Tag tag) const {
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

double UIThreadRenderer::numericProp(Tag tag, const char* name) const {
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

std::array<double, 4> UIThreadRenderer::borderWidths(Tag tag) const {
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

bool UIThreadRenderer::isPositioned(Tag tag) const {
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

bool UIThreadRenderer::isCommitting() const {
  return insideCommit_;
}

bool UIThreadRenderer::isTextComponentName(const std::string& componentName) {
  return isParagraphName(componentName) || isVirtualTextName(componentName);
}

bool UIThreadRenderer::isRawTextComponentName(
    const std::string& componentName) {
  return componentName == "RCTRawText" || componentName == "RawText";
}

bool UIThreadRenderer::isTextNode(Tag tag) const {
  const auto* node = find(tag);
  return node != nullptr && node->textRoot;
}

Tag UIThreadRenderer::parentOf(Tag tag) const {
  return parentOfAny(tag);
}

const char* UIThreadRenderer::internComponentName(
    const std::string& name) const {
  // A text fragment's parent view may outlive its renderer node.
  return internedComponentNames_.insert(name).first->c_str();
}

ShadowView UIThreadRenderer::shadowView(Tag tag) const {
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
    view.componentName = internComponentName("RCTRawText");
  }
  return view;
}

} // namespace facebook::react
#endif // UI_THREAD_RENDERER
