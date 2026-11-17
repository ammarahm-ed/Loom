/* Copyright (c) Meta Platforms, Inc. and affiliates. */

// LoomRenderer: props and style. The style table and Yoga style parsing, prop merging,
// role flags, and the animated-prop classification.

#include "LoomRendererShared.h"

#include <react/renderer/components/view/HostPlatformViewTraitsInitializer.h>
#include <react/renderer/components/view/ViewProps.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include <react/renderer/core/RawPropsParser.h>
#include <react/utils/ContextContainer.h>

#if LOOM_RENDERER
namespace facebook::react {

namespace {

#pragma mark - Prop readers




#pragma mark - Style values

/**
 * React expresses a Yoga length in one of three ways: a number in DIP, a
 * percentage string, or `auto`.
 */
struct LengthValue {
  float value{0};
  bool percent{false};
  bool isAuto{false};
  bool ok{false};
};

LengthValue parseLength(const folly::dynamic& value) {
  LengthValue result;
  if (value.isNumber()) {
    result.value = static_cast<float>(value.asDouble());
    result.ok = true;
    return result;
  }
  if (value.isString()) {
    const auto string = value.getString();
    if (string == "auto") {
      result.isAuto = true;
      result.ok = true;
      return result;
    }
    if (!string.empty() && string.back() == '%') {
      try {
        result.value = std::stof(string.substr(0, string.size() - 1));
        result.percent = true;
        result.ok = true;
      } catch (const std::exception&) {
        result.ok = false;
      }
    }
  }
  return result;
}

/** One style prop name and the Yoga edge slot it feeds. */
struct StylePropEntry {
  const char* name;
  LoomStyleKey key;
};

/**
 * Every style prop this renderer understands. Base props come first and
 * logical-property aliases after them, so an alias wins when both are present -
 * the same precedence React Native's own props parser uses.
 */
constexpr std::array<StylePropEntry, 67> kStyleProps = {{
    {"width", LoomStyleKey::Width},
    {"height", LoomStyleKey::Height},
    {"minWidth", LoomStyleKey::MinWidth},
    {"minHeight", LoomStyleKey::MinHeight},
    {"maxWidth", LoomStyleKey::MaxWidth},
    {"maxHeight", LoomStyleKey::MaxHeight},
    {"flexBasis", LoomStyleKey::FlexBasis},
    {"flex", LoomStyleKey::Flex},
    {"flexGrow", LoomStyleKey::FlexGrow},
    {"flexShrink", LoomStyleKey::FlexShrink},
    {"aspectRatio", LoomStyleKey::AspectRatio},
    {"gap", LoomStyleKey::GapAll},
    {"rowGap", LoomStyleKey::GapRow},
    {"columnGap", LoomStyleKey::GapColumn},
    {"margin", LoomStyleKey::MarginAll},
    {"marginHorizontal", LoomStyleKey::MarginHorizontal},
    {"marginVertical", LoomStyleKey::MarginVertical},
    {"marginStart", LoomStyleKey::MarginStart},
    {"marginEnd", LoomStyleKey::MarginEnd},
    {"marginTop", LoomStyleKey::MarginTop},
    {"marginRight", LoomStyleKey::MarginRight},
    {"marginBottom", LoomStyleKey::MarginBottom},
    {"marginLeft", LoomStyleKey::MarginLeft},
    {"padding", LoomStyleKey::PaddingAll},
    {"paddingHorizontal", LoomStyleKey::PaddingHorizontal},
    {"paddingVertical", LoomStyleKey::PaddingVertical},
    {"paddingStart", LoomStyleKey::PaddingStart},
    {"paddingEnd", LoomStyleKey::PaddingEnd},
    {"paddingTop", LoomStyleKey::PaddingTop},
    {"paddingRight", LoomStyleKey::PaddingRight},
    {"paddingBottom", LoomStyleKey::PaddingBottom},
    {"paddingLeft", LoomStyleKey::PaddingLeft},
    {"border", LoomStyleKey::BorderAll},
    {"borderWidth", LoomStyleKey::BorderAll},
    {"borderHorizontalWidth", LoomStyleKey::BorderHorizontal},
    {"borderVerticalWidth", LoomStyleKey::BorderVertical},
    {"borderStartWidth", LoomStyleKey::BorderStart},
    {"borderEndWidth", LoomStyleKey::BorderEnd},
    {"borderTopWidth", LoomStyleKey::BorderTop},
    {"borderRightWidth", LoomStyleKey::BorderRight},
    {"borderBottomWidth", LoomStyleKey::BorderBottom},
    {"borderLeftWidth", LoomStyleKey::BorderLeft},
    {"inset", LoomStyleKey::PositionAll},
    {"insetInline", LoomStyleKey::PositionHorizontal},
    {"insetBlock", LoomStyleKey::PositionVertical},
    {"start", LoomStyleKey::PositionStart},
    {"end", LoomStyleKey::PositionEnd},
    {"top", LoomStyleKey::PositionTop},
    {"right", LoomStyleKey::PositionRight},
    {"bottom", LoomStyleKey::PositionBottom},
    {"left", LoomStyleKey::PositionLeft},
    // Aliases.
    {"marginInline", LoomStyleKey::MarginHorizontal},
    {"marginInlineStart", LoomStyleKey::MarginStart},
    {"marginInlineEnd", LoomStyleKey::MarginEnd},
    {"marginBlock", LoomStyleKey::MarginVertical},
    {"marginBlockStart", LoomStyleKey::MarginTop},
    {"marginBlockEnd", LoomStyleKey::MarginBottom},
    {"paddingInline", LoomStyleKey::PaddingHorizontal},
    {"paddingInlineStart", LoomStyleKey::PaddingStart},
    {"paddingInlineEnd", LoomStyleKey::PaddingEnd},
    {"paddingBlock", LoomStyleKey::PaddingVertical},
    {"paddingBlockStart", LoomStyleKey::PaddingTop},
    {"paddingBlockEnd", LoomStyleKey::PaddingBottom},
    {"insetInlineStart", LoomStyleKey::PositionStart},
    {"insetInlineEnd", LoomStyleKey::PositionEnd},
    {"insetBlockStart", LoomStyleKey::PositionTop},
    {"insetBlockEnd", LoomStyleKey::PositionBottom},
}};

/**
 * Component names as the host view managers know them. JS already sends host
 * names, so this only has to translate the C++ names the shadow tree used,
 * mirroring `FabricNameComponentMapping` on the Java side.
 */
/**
 * Merges a `style` object (or an array of them) into the top level of `props`.
 *
 * That flat shape is what React's own attribute payload produces before props
 * reach the host, so everything downstream - Yoga style parsing included - only
 * ever reads the top level. Keys are merged individually rather than replaced,
 * so a patch touching one field cannot drop the others, and a `null` value is
 * kept as an explicit reset.
 */
void mergeStyleInto(folly::dynamic& props, const folly::dynamic& style) {
  if (style.isObject()) {
    for (const auto& entry : style.items()) {
      if (entry.first.isString()) {
        props[entry.first] = entry.second;
      }
    }
    return;
  }
  if (style.isArray()) {
    for (const auto& entry : style) {
      mergeStyleInto(props, entry);
    }
    return;
  }
  if (style.isNull()) {
    // React's attribute payload emits an explicit null for every style key it
    // clears, paint keys included, so a `style: null` payload normally arrives
    // with those nulls already alongside it. This branch is the safety net for
    // the keys this renderer parses itself (Yoga style), which must be reset
    // even if a caller sends the bare marker; keys this layer does not
    // understand are left to the host payload, exactly like every other paint
    // prop.
    for (const auto& entry : kStyleProps) {
      if (props.find(entry.name) != props.items().end()) {
        props[entry.name] = nullptr;
      }
    }
  }
}

} // namespace

namespace loom::internal {

folly::dynamic hoistProps(const folly::dynamic& props) {
  if (!props.isObject() || props.count("style") == 0) {
    return props.isObject() ? props : folly::dynamic::object();
  }
  folly::dynamic flattened = folly::dynamic::object();
  for (const auto& item : props.items()) {
    if (!item.first.isString()) {
      continue;
    }
    if (item.first.getString() == "style") {
      if (item.second.isNull()) {
        // Preserve the reset marker until it can be merged against the
        // existing live props; there are no style keys to erase yet here.
        flattened["style"] = nullptr;
      } else {
        mergeStyleInto(flattened, item.second);
      }
    } else {
      flattened[item.first] = item.second;
    }
  }
  return flattened;
}

bool isConservativePaintOnlyPatch(const folly::dynamic& patch) {
  const auto flattened = hoistProps(patch);
  if (!flattened.isObject()) {
    return false;
  }
  for (const auto& item : flattened.items()) {
    if (!item.first.isString()) {
      return false;
    }
    const auto& key = item.first.getString();
    if (key != "backgroundColor" && key != "opacity") {
      return false;
    }
  }
  return true;
}

} // namespace loom::internal

namespace {

/** O(1) membership test for `kStyleProps` names. */
bool isStylePropName(const std::string& key) {
  static const std::unordered_set<std::string> names = [] {
    std::unordered_set<std::string> set;
    for (const auto& entry : kStyleProps) {
      set.insert(entry.name);
    }
    return set;
  }();
  return names.count(key) != 0;
}

bool isLayoutOnlyView(bool isBox, const folly::dynamic& props) {
  if (!isBox) {
    return false;
  }
  const auto* collapsable = findProp(props, "collapsable");
  if (collapsable != nullptr && collapsable->isBool() &&
      !collapsable->asBool()) {
    return false;
  }
  const auto* collapsableChildren = findProp(props, "collapsableChildren");
  if (collapsableChildren != nullptr && collapsableChildren->isBool() &&
      !collapsableChildren->asBool()) {
    return false;
  }
  for (const auto& item : props.items()) {
    if (!item.first.isString()) {
      return false;
    }
    const auto& key = item.first.getString();
    if (key == "onLayout" || key == "collapsable" ||
        key == "collapsableChildren" || key == "direction" ||
        key == "boxSizing" || key == "display" || key == "flexDirection" ||
        key == "flexWrap" || key == "justifyContent" || key == "alignItems" ||
        key == "alignContent" || key == "alignSelf" || key == "position" ||
        key == "positionType") {
      continue;
    }
    if (isStylePropName(key) && key.rfind("border", 0) != 0) {
      continue;
    }
    // Unknown props are native-visible by default. This intentionally keeps
    // accessibility, touch, clipping, transforms and test/automation props
    // represented by a real Android View.
    return false;
  }
  // The platform's own say, as ViewShadowNode asks it: a platform whose
  // HostPlatformViewTraitsInitializer makes these props form a view or a
  // stacking context keeps the view, whatever the props above allow.
  static const RawPropsParser parser = [] {
    RawPropsParser prepared;
    prepared.prepare<ViewProps>();
    return prepared;
  }();
  static const ContextContainer contextContainer;
  RawProps rawProps(props);
  rawProps.parse(parser);
  const ViewProps viewProps(
      PropsParserContext{-1, contextContainer}, ViewProps{}, rawProps);
  return !HostPlatformViewTraitsInitializer::formsStackingContext(viewProps) &&
      !HostPlatformViewTraitsInitializer::formsView(viewProps);
}

} // namespace

/**
 * The animation fast lane's key table (design §3.2). It decides per key, not
 * per patch: `{transform, width}` splits into a paint key and a layout key.
 *
 * Nothing listed as paint is read by Yoga, so folding such a key into the
 * merged props without a style rebuild is exact — in Fabric, too, a transform
 * never triggers layout. The one derived value that depends on it, the mount
 * diff's overflow inset, is recomputed on the next real commit.
 */
AnimatedPropClass classifyAnimatedPropKey(const std::string& key) {
  if (key == "opacity" || key == "transform" || key == "transformOrigin" ||
      key == "backgroundColor" || key == "tintColor" ||
      key == "overlayColor" || key == "elevation") {
    return AnimatedPropClass::Paint;
  }
  // `borderColor*` and every `border*Radius` are paint on both platforms, while
  // `borderWidth*` is layout because it changes the box. `zIndex` also takes
  // the layout path because child reordering requires a mounting diff.
  if (key.rfind("border", 0) == 0 &&
      (key.find("Color") != std::string::npos ||
       key.find("Radius") != std::string::npos)) {
    return AnimatedPropClass::Paint;
  }
  if (key.rfind("shadow", 0) == 0) {
    return AnimatedPropClass::Paint;
  }
  // Text keys change font shaping or text measurement, so they cannot be
  // folded: they take the ordinary commit path (design §3.2 "Text").
  if (key.rfind("font", 0) == 0 || key == "lineHeight" ||
      key == "letterSpacing" || key == "textAlign" ||
      key == "textAlignVertical" || key == "textTransform" ||
      key == "textDecorationLine" || key == "textShadowColor" ||
      key == "textShadowOffset" || key == "textShadowRadius" ||
      key == "allowFontScaling" || key == "numberOfLines") {
    return AnimatedPropClass::Text;
  }
  return AnimatedPropClass::Layout;
}

ClassifiedAnimatedProps classifyAnimatedProps(const folly::dynamic& props) {
  ClassifiedAnimatedProps classified;
  // The host sees hoisted props, so the classes are decided on the same keys.
  // A flat payload (the Native Animated case) is read in place, not copied.
  folly::dynamic hoisted;
  const bool flat = isFlatProps(props);
  if (!flat) {
    hoisted = hoistProps(props);
  }
  const folly::dynamic& flattened = flat ? props : hoisted;
  if (!flattened.isObject()) {
    return classified;
  }
  for (const auto& item : flattened.items()) {
    if (!item.first.isString()) {
      classified.layout[item.first] = item.second;
      continue;
    }
    const auto& key = item.first.getString();
    switch (classifyAnimatedPropKey(key)) {
      case AnimatedPropClass::Paint:
        classified.paint[key] = item.second;
        break;
      case AnimatedPropClass::Text:
        // The caller routes the whole payload through the normal commit path,
        // so the split itself does not have to be exact here.
        classified.text = true;
        classified.layout[key] = item.second;
        break;
      case AnimatedPropClass::Layout:
        classified.layout[key] = item.second;
        break;
    }
  }
  return classified;
}

void LoomRenderer::mergeProps(Node& node, const folly::dynamic& patch) {
  if (!patch.isObject() || !node.props.isObject()) {
    return;
  }
  for (const auto& item : patch.items()) {
    const auto& key = item.first;
    if (!key.isString()) {
      continue;
    }
    // [LoomMemory] The delta is a second copy of every changed key; only
    // components that ship diffs ever read it.
    if (node.collectPropsDelta && !node.propsDelta.isObject()) {
      node.propsDelta = folly::dynamic::object();
    }
    if (key.getString() == "style") {
      mergeStyleInto(node.props, item.second);
      if (node.collectPropsDelta) {
        mergeStyleInto(node.propsDelta, item.second);
      }
      ++node.propsGeneration;
      continue;
    }
    // A null value is meaningful: it resets the prop to its default. It stays
    // in the map, because a prop that is merely absent would never reach the
    // host and the old value would linger.
    //
    // This is coupled to updates carrying the whole merged map: the null is the
    // only thing that tells the host to clear the previous value, so pruning
    // applied null keys is safe only once updates carry a diff instead.
    node.props[key] = item.second;
    if (node.collectPropsDelta) {
      node.propsDelta[key] = item.second;
    }
    ++node.propsGeneration;
  }
  node.propsDirty = true;
  // `roleFlagsValid` is cleared on every merge, including the paint-only path,
  // rather than reasoning about which keys can change layout-only-ness. The
  // recomputation is per changed node per frame; treating it as per commit for
  // every node is what made it expensive.
  //
  // This is load-bearing and the suite proves it: with this line removed,
  // `PaintPatchCannotLeaveLayoutOnlyNodeUnmaterialized` fails.
  node.roleFlagsValid = false;
  markNeedsPrepare(node);
}

/**
 * Rebuilds the Yoga style from the node's merged props.
 *
 * Always derived from the full prop set, never from a diff, so a key that a
 * previous patch happened to set cannot linger in the Yoga node.
 */
void LoomRenderer::refreshStyle(Node& node) {
  static_assert(
      size_t(LoomStyleKey::Count) == 50,
      "Style key table and enum are out of sync");
  static_assert(
      kStyleProps.size() == 67,
      "Style prop table and LoomStyleKey are out of sync");

  LayoutStyle next;
  for (const auto& entry : kStyleProps) {
    const auto* value = findProp(node.props, entry.name);
    if (value == nullptr) {
      continue;
    }
    const auto parsed = parseLength(*value);
    if (!parsed.ok) {
      continue;
    }
    StyleValue styleValue;
    styleValue.value = parsed.value;
    styleValue.unit =
        parsed.percent ? StyleValue::Unit::Percent : StyleValue::Unit::Point;
    next.values[size_t(entry.key)] = styleValue;
    next.autoFlags[size_t(entry.key)] = parsed.isAuto;
  }
  if (node.style != next) {
    node.style = std::move(next);
    node.styleDirty = true;
  }
  auto nextDirection = YGDirectionInherit;
  const auto& direction = stringProp(node.props, "direction");
  if (direction == "ltr") {
    nextDirection = YGDirectionLTR;
  } else if (direction == "rtl") {
    nextDirection = YGDirectionRTL;
  }
  if (node.direction != nextDirection) {
    node.direction = nextDirection;
    node.styleDirty = true;
  }
  auto nextBoxSizing = YGBoxSizingBorderBox;
  if (stringProp(node.props, "boxSizing") == "content-box") {
    nextBoxSizing = YGBoxSizingContentBox;
  }
  if (node.boxSizing != nextBoxSizing) {
    node.boxSizing = nextBoxSizing;
    node.styleDirty = true;
  }
  node.displayNone =
      node.offscreenHidden || stringProp(node.props, "display") == "none";
}

namespace {

/** Keys whose values feed layout parsing or the layout-related role flags. */
bool isLayoutShapeKey(const std::string& key) {
  static const std::unordered_set<std::string> names = [] {
    std::unordered_set<std::string> set;
    for (const auto& entry : kStyleProps) {
      set.insert(entry.name);
    }
    for (const char* extra :
         {"direction", "boxSizing", "display", "flexDirection", "flexWrap",
          "justifyContent", "alignItems", "alignContent", "alignSelf",
          "position", "positionType", "overflow", "zIndex", "collapsable",
          "collapsableChildren", "onLayout"}) {
      set.insert(extra);
    }
    return set;
  }();
  return names.count(key) != 0;
}

constexpr size_t kLayoutShapeCacheLimit = 256;

} // namespace

/**
 * [LoomPerf item 2] `refreshStyle` for a freshly created node. Rows are
 * near-identical, so the parse is looked up by a structural hash of the
 * layout-relevant props and verified entry by entry on a hit; a node whose
 * props cannot be summarised cheaply falls back to the plain path.
 */
void LoomRenderer::refreshStyleCached(Node& node) {
  if (node.hostOwned || !node.props.isObject()) {
    refreshStyle(node);
    return;
  }
  std::vector<std::pair<const std::string*, const folly::dynamic*>> picked;
  picked.reserve(16);
  bool hasOtherKeys = false;
  uint64_t entriesHash = 0;
  for (const auto& item : node.props.items()) {
    if (!item.first.isString()) {
      refreshStyle(node);
      return;
    }
    const auto& key = item.first.getString();
    if (!isLayoutShapeKey(key)) {
      hasOtherKeys = true;
      continue;
    }
    const auto& value = item.second;
    if (value.isObject() || value.isArray()) {
      refreshStyle(node);
      return;
    }
    picked.emplace_back(&key, &value);
    // Order independent, so map iteration order cannot split a shape.
    entriesHash += (std::hash<std::string>{}(key) * 0x9E3779B97F4A7C15ull) ^
        (static_cast<uint64_t>(value.hash()) * 0xC2B2AE3D27D4EB4Full +
         static_cast<uint64_t>(value.type()));
  }
  const uint64_t hash = entriesHash * 31 +
      std::hash<std::string>{}(node.componentName) + (hasOtherKeys ? 7 : 0) +
      picked.size();

  const auto matches = [&](const LayoutShape& shape) {
    if (shape.hasOtherKeys != hasOtherKeys ||
        shape.entries.size() != picked.size() ||
        shape.componentName != node.componentName) {
      return false;
    }
    for (const auto& entry : picked) {
      bool found = false;
      for (const auto& cached : shape.entries) {
        if (cached.first == *entry.first) {
          found = cached.second.type() == entry.second->type() &&
              cached.second == *entry.second;
          break;
        }
      }
      if (!found) {
        return false;
      }
    }
    return true;
  };

  auto cached = shapeCache_.find(hash);
  if (cached != shapeCache_.end() && matches(*cached->second)) {
    const auto& shape = *cached->second;
    node.style = shape.style;
    node.direction = shape.direction;
    node.boxSizing = shape.boxSizing;
    node.displayNone = shape.displayNone || node.offscreenHidden;
    node.styleDirty = true;
    node.shape = cached->second;
    node.shapeGeneration = node.propsGeneration;
    return;
  }

  refreshStyle(node);
  auto shape = std::make_shared<LayoutShape>();
  shape->entries.reserve(picked.size());
  for (const auto& entry : picked) {
    shape->entries.emplace_back(*entry.first, *entry.second);
  }
  shape->componentName = node.componentName;
  shape->hasOtherKeys = hasOtherKeys;
  shape->style = node.style;
  shape->direction = node.direction;
  shape->boxSizing = node.boxSizing;
  shape->displayNone = stringProp(node.props, "display") == "none";
  shape->layoutOnly = isLayoutOnlyView(
      node.componentRoles.has(LoomComponentTable::Box), node.props);
  shape->zIndex = numberProp(node.props, "zIndex").value_or(0);
  if (const auto* overflow = findProp(node.props, "overflow");
      overflow != nullptr && overflow->isString()) {
    const auto& value = overflow->getString();
    shape->clipsOverflow = value == "hidden" || value == "scroll";
  }
  if (cached == shapeCache_.end()) {
    if (shapeCache_.size() >= kLayoutShapeCacheLimit) {
      shapeCache_.clear();
    }
    shapeCache_.emplace(hash, shape);
  }
  node.shape = std::move(shape);
  node.shapeGeneration = node.propsGeneration;
}

void LoomRenderer::applyYogaProps(Node& node) {
  const auto yogaNode = node.yogaNode;
  if (yogaNode == nullptr) {
    return;
  }
  const auto& style = node.style;

  const auto at = [&style](LoomStyleKey key) {
    const auto index = size_t(key);
    return std::pair<const StyleValue*, bool>{
        style.values[index].has_value() ? &*style.values[index] : nullptr,
        style.autoFlags[index]};
  };

  /** Writes a length, or resets the slot when this revision does not set it. */
  const auto setLength = [](YGNodeRef yogaNode,
                            const StyleValue* value,
                            bool isAuto,
                            void (*setPoint)(YGNodeRef, float),
                            void (*setPercent)(YGNodeRef, float),
                            void (*setAuto)(YGNodeRef)) {
    if (value == nullptr) {
      setPoint(yogaNode, YGUndefined);
    } else if (isAuto && setAuto != nullptr) {
      setAuto(yogaNode);
    } else if (value->unit == StyleValue::Unit::Percent) {
      setPercent(yogaNode, value->value);
    } else {
      setPoint(yogaNode, value->value);
    }
  };

  const auto setEdge = [&](LoomStyleKey key, YGEdge edge, int kind) {
    const auto [value, isAuto] = at(key);
    const auto percent =
        value != nullptr && value->unit == StyleValue::Unit::Percent;
    switch (kind) {
      case 0:
        if (value == nullptr) {
          YGNodeStyleSetMargin(yogaNode, edge, YGUndefined);
        } else if (isAuto) {
          YGNodeStyleSetMarginAuto(yogaNode, edge);
        } else if (percent) {
          YGNodeStyleSetMarginPercent(yogaNode, edge, value->value);
        } else {
          YGNodeStyleSetMargin(yogaNode, edge, value->value);
        }
        break;
      case 1:
        if (value == nullptr) {
          YGNodeStyleSetPadding(yogaNode, edge, YGUndefined);
        } else if (percent) {
          YGNodeStyleSetPaddingPercent(yogaNode, edge, value->value);
        } else {
          YGNodeStyleSetPadding(yogaNode, edge, value->value);
        }
        break;
      case 2:
        // Yoga has no percent borders.
        YGNodeStyleSetBorder(
            yogaNode, edge, value == nullptr ? YGUndefined : value->value);
        break;
      default:
        if (value == nullptr) {
          YGNodeStyleSetPosition(yogaNode, edge, YGUndefined);
        } else if (isAuto) {
          YGNodeStyleSetPositionAuto(yogaNode, edge);
        } else if (percent) {
          YGNodeStyleSetPositionPercent(yogaNode, edge, value->value);
        } else {
          YGNodeStyleSetPosition(yogaNode, edge, value->value);
        }
        break;
    }
  };

  {
    const auto [value, isAuto] = at(LoomStyleKey::Width);
    setLength(
        yogaNode,
        value,
        isAuto,
        YGNodeStyleSetWidth,
        YGNodeStyleSetWidthPercent,
        YGNodeStyleSetWidthAuto);
  }
  {
    const auto [value, isAuto] = at(LoomStyleKey::Height);
    setLength(
        yogaNode,
        value,
        isAuto,
        YGNodeStyleSetHeight,
        YGNodeStyleSetHeightPercent,
        YGNodeStyleSetHeightAuto);
  }
  {
    const auto [value, isAuto] = at(LoomStyleKey::MinWidth);
    (void)isAuto;
    setLength(
        yogaNode,
        value,
        false,
        YGNodeStyleSetMinWidth,
        YGNodeStyleSetMinWidthPercent,
        nullptr);
  }
  {
    const auto [value, isAuto] = at(LoomStyleKey::MinHeight);
    (void)isAuto;
    setLength(
        yogaNode,
        value,
        false,
        YGNodeStyleSetMinHeight,
        YGNodeStyleSetMinHeightPercent,
        nullptr);
  }
  {
    const auto [value, isAuto] = at(LoomStyleKey::MaxWidth);
    (void)isAuto;
    setLength(
        yogaNode,
        value,
        false,
        YGNodeStyleSetMaxWidth,
        YGNodeStyleSetMaxWidthPercent,
        nullptr);
  }
  {
    const auto [value, isAuto] = at(LoomStyleKey::MaxHeight);
    (void)isAuto;
    setLength(
        yogaNode,
        value,
        false,
        YGNodeStyleSetMaxHeight,
        YGNodeStyleSetMaxHeightPercent,
        nullptr);
  }
  // Keep `flex` as Yoga's shorthand. With the Fabric-compatible config Yoga
  // resolves positive flex to grow/zero-basis and negative flex to shrink.
  // Explicit longhands retain Yoga's normal precedence over the shorthand.
  if (auto flex = numberProp(node.props, "flex"); flex.has_value()) {
    YGNodeStyleSetFlex(yogaNode, *flex);
  } else {
    YGNodeStyleSetFlex(yogaNode, YGUndefined);
  }

  if (findProp(node.props, "flexBasis") != nullptr) {
    const auto [value, isAuto] = at(LoomStyleKey::FlexBasis);
    setLength(
        yogaNode,
        value,
        isAuto,
        YGNodeStyleSetFlexBasis,
        YGNodeStyleSetFlexBasisPercent,
        YGNodeStyleSetFlexBasisAuto);
  } else {
    YGNodeStyleSetFlexBasis(yogaNode, YGUndefined);
  }

  const auto setNumber = [&](const char* name,
                             void (*setter)(YGNodeRef, float)) {
    if (auto value = numberProp(node.props, name)) {
      setter(yogaNode, *value);
    } else {
      setter(yogaNode, YGUndefined);
    }
  };
  if (findProp(node.props, "flexGrow") != nullptr) {
    setNumber("flexGrow", YGNodeStyleSetFlexGrow);
  } else {
    YGNodeStyleSetFlexGrow(yogaNode, YGUndefined);
  }
  if (findProp(node.props, "flexShrink") != nullptr) {
    setNumber("flexShrink", YGNodeStyleSetFlexShrink);
  } else {
    YGNodeStyleSetFlexShrink(yogaNode, YGUndefined);
  }
  // `aspectRatio` also accepts a CSS ratio string ("16/9") under the same
  // feature flag Fabric's `convertAspectRatio` uses, and a degenerate ratio
  // (a zero denominator) is ignored there too, so it is ignored here.
  if (const auto* aspectRatio = findProp(node.props, "aspectRatio");
      aspectRatio != nullptr && aspectRatio->isString() &&
      ReactNativeFeatureFlags::enableNativeCSSParsing()) {
    const auto& text = aspectRatio->getString();
    const auto slash = text.find('/');
    if (slash != std::string::npos) {
      const auto numerator = std::strtof(text.c_str(), nullptr);
      const auto denominator = std::strtof(text.c_str() + slash + 1, nullptr);
      if (denominator != 0.0f && std::isfinite(numerator) &&
          std::isfinite(denominator)) {
        YGNodeStyleSetAspectRatio(yogaNode, numerator / denominator);
      } else {
        YGNodeStyleSetAspectRatio(yogaNode, YGUndefined);
      }
    } else {
      // A bare number written as a string still behaves as a number.
      setNumber("aspectRatio", YGNodeStyleSetAspectRatio);
    }
  } else {
    setNumber("aspectRatio", YGNodeStyleSetAspectRatio);
  }

  // Edges.
  setEdge(LoomStyleKey::MarginAll, YGEdgeAll, 0);
  setEdge(LoomStyleKey::MarginHorizontal, YGEdgeHorizontal, 0);
  setEdge(LoomStyleKey::MarginVertical, YGEdgeVertical, 0);
  setEdge(LoomStyleKey::MarginStart, YGEdgeStart, 0);
  setEdge(LoomStyleKey::MarginEnd, YGEdgeEnd, 0);
  setEdge(LoomStyleKey::MarginTop, YGEdgeTop, 0);
  setEdge(LoomStyleKey::MarginRight, YGEdgeRight, 0);
  setEdge(LoomStyleKey::MarginBottom, YGEdgeBottom, 0);
  setEdge(LoomStyleKey::MarginLeft, YGEdgeLeft, 0);

  setEdge(LoomStyleKey::PaddingAll, YGEdgeAll, 1);
  setEdge(LoomStyleKey::PaddingHorizontal, YGEdgeHorizontal, 1);
  setEdge(LoomStyleKey::PaddingVertical, YGEdgeVertical, 1);
  setEdge(LoomStyleKey::PaddingStart, YGEdgeStart, 1);
  setEdge(LoomStyleKey::PaddingEnd, YGEdgeEnd, 1);
  setEdge(LoomStyleKey::PaddingTop, YGEdgeTop, 1);
  setEdge(LoomStyleKey::PaddingRight, YGEdgeRight, 1);
  setEdge(LoomStyleKey::PaddingBottom, YGEdgeBottom, 1);
  setEdge(LoomStyleKey::PaddingLeft, YGEdgeLeft, 1);

  setEdge(LoomStyleKey::BorderAll, YGEdgeAll, 2);
  setEdge(LoomStyleKey::BorderHorizontal, YGEdgeHorizontal, 2);
  setEdge(LoomStyleKey::BorderVertical, YGEdgeVertical, 2);
  setEdge(LoomStyleKey::BorderStart, YGEdgeStart, 2);
  setEdge(LoomStyleKey::BorderEnd, YGEdgeEnd, 2);
  setEdge(LoomStyleKey::BorderTop, YGEdgeTop, 2);
  setEdge(LoomStyleKey::BorderRight, YGEdgeRight, 2);
  setEdge(LoomStyleKey::BorderBottom, YGEdgeBottom, 2);
  setEdge(LoomStyleKey::BorderLeft, YGEdgeLeft, 2);

  setEdge(LoomStyleKey::PositionAll, YGEdgeAll, 3);
  setEdge(LoomStyleKey::PositionHorizontal, YGEdgeHorizontal, 3);
  setEdge(LoomStyleKey::PositionVertical, YGEdgeVertical, 3);
  setEdge(LoomStyleKey::PositionStart, YGEdgeStart, 3);
  setEdge(LoomStyleKey::PositionEnd, YGEdgeEnd, 3);
  setEdge(LoomStyleKey::PositionTop, YGEdgeTop, 3);
  setEdge(LoomStyleKey::PositionRight, YGEdgeRight, 3);
  setEdge(LoomStyleKey::PositionBottom, YGEdgeBottom, 3);
  setEdge(LoomStyleKey::PositionLeft, YGEdgeLeft, 3);

  const auto setGap = [&](LoomStyleKey key, YGGutter gutter) {
    const auto [value, isAuto] = at(key);
    (void)isAuto;
    if (value == nullptr) {
      YGNodeStyleSetGap(yogaNode, gutter, YGUndefined);
    } else if (value->unit == StyleValue::Unit::Percent) {
      YGNodeStyleSetGapPercent(yogaNode, gutter, value->value);
    } else {
      YGNodeStyleSetGap(yogaNode, gutter, value->value);
    }
  };
  setGap(LoomStyleKey::GapAll, YGGutterAll);
  setGap(LoomStyleKey::GapRow, YGGutterRow);
  setGap(LoomStyleKey::GapColumn, YGGutterColumn);

  // Enumerated styles. Yoga's C API has no "unset" for these, so an absent prop
  // is written back as React Native's default.
  const auto& flexDirection = stringProp(node.props, "flexDirection");
  if (flexDirection == "row") {
    YGNodeStyleSetFlexDirection(yogaNode, YGFlexDirectionRow);
  } else if (flexDirection == "row-reverse") {
    YGNodeStyleSetFlexDirection(yogaNode, YGFlexDirectionRowReverse);
  } else if (flexDirection == "column-reverse") {
    YGNodeStyleSetFlexDirection(yogaNode, YGFlexDirectionColumnReverse);
  } else if (flexDirection.empty() || flexDirection == "column") {
    YGNodeStyleSetFlexDirection(yogaNode, YGFlexDirectionColumn);
  } else {
    // Invalid enum values are an explicit reset to RN's default, never a
    // partially retained value from the previous revision.
    YGNodeStyleSetFlexDirection(yogaNode, YGFlexDirectionColumn);
  }

  const auto& flexWrap = stringProp(node.props, "flexWrap");
  if (flexWrap == "wrap") {
    YGNodeStyleSetFlexWrap(yogaNode, YGWrapWrap);
  } else if (flexWrap == "wrap-reverse") {
    YGNodeStyleSetFlexWrap(yogaNode, YGWrapWrapReverse);
  } else if (flexWrap.empty() || flexWrap == "nowrap") {
    YGNodeStyleSetFlexWrap(yogaNode, YGWrapNoWrap);
  } else {
    YGNodeStyleSetFlexWrap(yogaNode, YGWrapNoWrap);
  }

  const auto& justifyContent = stringProp(node.props, "justifyContent");
  if (justifyContent == "center") {
    YGNodeStyleSetJustifyContent(yogaNode, YGJustifyCenter);
  } else if (justifyContent == "flex-end") {
    YGNodeStyleSetJustifyContent(yogaNode, YGJustifyFlexEnd);
  } else if (justifyContent == "space-between") {
    YGNodeStyleSetJustifyContent(yogaNode, YGJustifySpaceBetween);
  } else if (justifyContent == "space-around") {
    YGNodeStyleSetJustifyContent(yogaNode, YGJustifySpaceAround);
  } else if (justifyContent == "space-evenly") {
    YGNodeStyleSetJustifyContent(yogaNode, YGJustifySpaceEvenly);
  } else if (justifyContent.empty() || justifyContent == "flex-start") {
    YGNodeStyleSetJustifyContent(yogaNode, YGJustifyFlexStart);
  } else {
    YGNodeStyleSetJustifyContent(yogaNode, YGJustifyFlexStart);
  }

  const auto alignFromString = [](const std::string& value) {
    if (value == "flex-start") {
      return YGAlignFlexStart;
    }
    if (value == "center") {
      return YGAlignCenter;
    }
    if (value == "flex-end") {
      return YGAlignFlexEnd;
    }
    if (value == "space-between") {
      return YGAlignSpaceBetween;
    }
    if (value == "space-around") {
      return YGAlignSpaceAround;
    }
    if (value == "space-evenly") {
      return YGAlignSpaceEvenly;
    }
    if (value == "baseline") {
      return YGAlignBaseline;
    }
    if (value == "stretch") {
      return YGAlignStretch;
    }
    return YGAlignStretch;
  };
  const auto& alignItems = stringProp(node.props, "alignItems");
  YGNodeStyleSetAlignItems(
      yogaNode,
      alignItems.empty() ? YGAlignStretch : alignFromString(alignItems));
  const auto& alignContent = stringProp(node.props, "alignContent");
  YGNodeStyleSetAlignContent(
      yogaNode,
      alignContent.empty() ? YGAlignFlexStart : alignFromString(alignContent));
  const auto& alignSelf = stringProp(node.props, "alignSelf");
  const bool hasExplicitAlignSelf = !alignSelf.empty() && alignSelf != "auto";
  YGNodeStyleSetAlignSelf(
      yogaNode,
      hasExplicitAlignSelf ? alignFromString(alignSelf) : YGAlignAuto);
  YGNodeStyleSetBoxSizing(yogaNode, node.boxSizing);

  auto position = stringProp(node.props, "position");
  if (position.empty()) {
    position = stringProp(node.props, "positionType");
  }
  if (position == "absolute") {
    YGNodeStyleSetPositionType(yogaNode, YGPositionTypeAbsolute);
  } else if (position == "static") {
    YGNodeStyleSetPositionType(yogaNode, YGPositionTypeStatic);
  } else {
    YGNodeStyleSetPositionType(yogaNode, YGPositionTypeRelative);
  }

  YGNodeStyleSetDisplay(
      yogaNode, node.displayNone ? YGDisplayNone : YGDisplayFlex);

  const auto& overflow = stringProp(node.props, "overflow");
  if (overflow == "hidden") {
    YGNodeStyleSetOverflow(yogaNode, YGOverflowHidden);
  } else if (overflow == "scroll") {
    YGNodeStyleSetOverflow(yogaNode, YGOverflowScroll);
  } else {
    YGNodeStyleSetOverflow(yogaNode, YGOverflowVisible);
  }

  node.styleDirty = false;
}

#pragma mark - Commit

void LoomRenderer::refreshRoleFlags(Node& node) {
  const auto& roles = node.componentRoles;
  node.roleIsText = roles.isText();
  node.roleIsParagraph = roles.isParagraph();
  node.roleIsTextInput = roles.textInput;
  node.roleStateKind = roles.stateKind;
  node.roleRequiresIntrinsicMeasurement =
      roles.has(LoomComponentTable::IntrinsicMeasurement);
  node.roleRequiresGenericStateWrapper =
      roles.has(LoomComponentTable::GenericStateWrapper);
  // The expensive one: see the field comment. Computed once per props version
  // rather than once per commit per node.
  const bool useShape = shapeValid(node);
  node.roleIsLayoutOnlyView =
      useShape ? node.shape->layoutOnly
               : isLayoutOnlyView(roles.has(LoomComponentTable::Box), node.props);
  // `display: contents` boxes no view of its own, whatever the component:
  // Fabric force-flattens it (ShadowNodeTraits::ForceFlattenView), and a host
  // view for it would be a 0x0 parent that Android routes no touches through.
  if (!node.roleIsLayoutOnlyView && !node.roleIsText && !node.roleIsParagraph &&
      !node.roleIsTextInput) {
    if (const auto* display = findProp(node.props, "display");
        display != nullptr && display->isString() &&
        display->getString() == "contents") {
      node.roleIsLayoutOnlyView = true;
    }
  }
  node.roleZIndex = useShape ? node.shape->zIndex
                             : numberProp(node.props, "zIndex").value_or(0);
  node.roleSendsPropDiffs = node.collectPropsDelta;
  if (useShape) {
    node.roleClipsOverflow = node.shape->clipsOverflow;
  } else if (const auto* overflow = findProp(node.props, "overflow");
             overflow != nullptr && overflow->isString()) {
    const auto& value = overflow->getString();
    node.roleClipsOverflow = value == "hidden" || value == "scroll";
  } else {
    node.roleClipsOverflow = false;
  }
  node.roleFlagsValid = true;
}

} // namespace facebook::react
#endif // LOOM_RENDERER
