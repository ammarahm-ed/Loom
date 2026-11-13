/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomRenderer.h"
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif
#include "LoomRendererInternals.h"
#include "LoomWorkerProfile.h"

#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/renderer/core/DynamicEventPayload.h>
#include <chrono>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <limits>
#include <tuple>
#include <utility>



#if LOOM_RENDERER
namespace facebook::react {
namespace {

using loom::isParagraphName;
using loom::isRawTextName;
using loom::isVirtualTextName;
using loom::findProp;
using loom::numberProp;
using loom::propsFromValue;
using loom::requireArguments;
using loom::stringProp;

/** `display: none` and visibility, matching `DisplayType` in the shadow tree.
 */
constexpr int kDisplayNone = 0;
constexpr int kDisplayFlex = 1;

/** `LayoutDirection` values as Android's `updateLayout` expects them. */
constexpr int kLayoutDirectionLTR = 1;
constexpr int kLayoutDirectionRTL = 2;

uint64_t childPairKey(Tag parent, Tag child) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(parent)) << 32) |
      static_cast<uint32_t>(child);
}

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

/**
 * The host-ready shape of a props payload: a flat object, with any nested
 * `style` hoisted into it. React already sends props in this shape, so the copy
 * is skipped whenever there is no `style` key.
 */
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

/** True when the payload has no nested `style`, so it is already host-ready. */
bool isFlatProps(const folly::dynamic& props) {
  return props.isObject() && props.count("style") == 0;
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

bool isLayoutOnlyView(
    const std::string& componentName,
    const folly::dynamic& props) {
  if (componentName != "View" && componentName != "RCTView") {
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
  return true;
}

/** Paragraph components: text roots that own a host view. */

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

LoomTextStateKind textStateKindFor(const std::string& name) {
  if (name == "RCTText") {
    return LoomTextStateKind::Text;
  }
  if (name == "Paragraph" || name == "Text") {
    return LoomTextStateKind::Paragraph;
  }
  if (name == "RCTSelectableText" || name == "SelectableParagraph") {
    return LoomTextStateKind::SelectableParagraph;
  }
  if (name.find("TextInput") != std::string::npos) {
    return LoomTextStateKind::TextInput;
  }
  return LoomTextStateKind::Other;
}

bool isTextInputName(const std::string& componentName) {
  return componentName.find("TextInput") != std::string::npos;
}

bool requiresGenericStateWrapper(const std::string& componentName) {
  return componentName == "RCTScrollView" ||
      componentName == "RCTHorizontalScrollView" ||
      componentName == "AndroidHorizontalScrollView" ||
      componentName == "AndroidNestedScrollView" ||
      componentName == "RCTSafeAreaView" || componentName == "RCTModalHostView";
}

bool requiresIntrinsicMeasurement(const std::string& componentName) {
  return componentName == "AndroidProgressBar" ||
      componentName == "RCTSwitch" || componentName == "AndroidSwitch" ||
      componentName == "RCTSlider";
}

/**
 * A deliberately conservative fallback used only when the platform cannot
 * measure text. Laying text out on an estimate clips or over-spaces it, so the
 * host measurer is always preferred.
 */
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

std::string LoomRenderer::hostComponentName(
    const std::string& componentName) {
  // Fabric name -> Android ViewManager name, mirroring
  // FabricNameComponentMapping.kt, which is the same direction. It is NOT
  // `componentNameByReactViewName`, which maps the other way (React view name ->
  // Fabric name, stripping the RCT prefix); using it here would leave every
  // entry reversed and no host view would resolve.
  //
  // The entries for components this repository does not itself ship
  // (ShimmeringView, TemplateView, Video, Map, Keyframes,
  // ImpressionTrackingView) are upstream's own, not leftovers: an app whose
  // ViewManager registers one of those names must resolve exactly as it does
  // under Fabric.
  if (componentName == "View")
    return "RCTView";
  if (componentName == "Image")
    return "RCTImageView";
  if (componentName == "ScrollView")
    return "RCTScrollView";
  if (componentName == "Paragraph")
    return "RCTText";
  if (componentName == "SelectableParagraph")
    return "RCTSelectableText";
  // LayoutConformance is a Yoga-only context node in the component registry;
  // there is no Android manager for its Fabric name. The direct Android host
  // needs the ordinary ViewManager for the box while Yoga handles layout.
  if (componentName == "LayoutConformance")
    return "RCTView";
  // "Text" is deliberately absent: that is the shadow-tree name of *nested*
  // text, which is a span inside the enclosing paragraph and must not turn
  // into a second text view. Only "Paragraph" is a text view.
  if (componentName == "RawText")
    return "RCTRawText";
  if (componentName == "AndroidHorizontalScrollView")
    return "RCTScrollView";
  if (componentName == "ActivityIndicatorView")
    return "AndroidProgressBar";
  if (componentName == "ShimmeringView")
    return "RKShimmeringView";
  if (componentName == "TemplateView")
    return "RCTTemplateView";
  if (componentName == "AxialGradientView")
    return "RCTAxialGradientView";
  if (componentName == "ModalHostView")
    return "RCTModalHostView";
  if (componentName == "Slider")
    return "RCTSlider";
  if (componentName == "Video")
    return "RCTVideo";
  if (componentName == "Map")
    return "RCTMap";
  if (componentName == "WebView")
    return "RCTWebView";
  if (componentName == "Keyframes")
    return "RCTKeyframes";
  if (componentName == "ImpressionTrackingView") {
    return "RCTImpressionTrackingView";
  }
  return componentName;
}

#pragma mark - LoomRenderer

LoomRenderer::LoomRenderer(
    SurfaceId surfaceId,
    NativeOperations operations)
    : surfaceId_(surfaceId),
      operations_(std::move(operations)),
      yogaConfig_(YGConfigNew()) {
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
}

#pragma mark Props

// Which components take only the changed keys on update (see `Node::roleSendsPropDiffs`).
// Depends on the component name alone, so `createNode` evaluates it once and `mergeProps`
// builds the delta only for nodes that will ship it.
static bool componentSendsPropDiffs(const std::string& name) {
#if defined(__APPLE__)
  return name.find("Image") == std::string::npos;
#else
  return name == "RCTView" || name == "RCTImageView" ||
      name == "RCTScrollView" || name == "AndroidHorizontalScrollView";
#endif
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
  node.displayNone = stringProp(node.props, "display") == "none";
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
    node.displayNone = shape.displayNone;
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
  shape->displayNone = node.displayNone;
  shape->layoutOnly = isLayoutOnlyView(node.componentName, node.props);
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
  const auto& name = node.componentName;
  node.roleIsText = isTextComponentName(name);
  node.roleIsParagraph = isParagraphName(name);
  node.roleIsTextInput = isTextInputName(name);
  node.roleStateKind = textStateKindFor(name);
  node.roleRequiresIntrinsicMeasurement = requiresIntrinsicMeasurement(name);
  node.roleRequiresGenericStateWrapper = requiresGenericStateWrapper(name);
  // The expensive one: see the field comment. Computed once per props version
  // rather than once per commit per node.
  const bool useShape = shapeValid(node);
  node.roleIsLayoutOnlyView =
      useShape ? node.shape->layoutOnly : isLayoutOnlyView(name, node.props);
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
#if defined(__APPLE__)
  // iOS applies an update as `cloneProps(oldProps, rawProps)`, which keeps every
  // key the raw props leave out - the same thing Fabric does with React's diff -
  // so every component can take only the changed keys. Image is the exception:
  // its state is rebuilt from the raw props the applier was given.
  node.roleSendsPropDiffs = name.find("Image") == std::string::npos;
#else
  // Text is left out: one of its managers reads the stored props in
  // updateState, which would then see only the last delta.
  node.roleSendsPropDiffs = name == "RCTView" || name == "RCTImageView" ||
      name == "RCTScrollView" || name == "AndroidHorizontalScrollView";
#endif
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

void LoomRenderer::prepare(
    Node& node,
    bool insideText,
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
          child->lastParentViewTag == childParentView.tag) {
        continue;
      }
      prepare(
          *child,
          childrenInsideText,
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
      hostComponentName(node.componentName),
      node.layoutOnlyNode ? folly::dynamic::object() : node.props,
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
#if defined(__ANDROID__) || defined(__APPLE__)
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
#endif
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
  for (size_t index = 0; index < nextCount; ++index) {
    if (stationaryChildren.count(childPairKey(node.tag, next[index])) != 0) {
      continue;
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

  if (insetsChanged) {
    pendingOperations_.append(DirectPaddingOperation{node.tag, contentInsets});
    pendingOperations_.append(
        DirectOverflowInsetOperation{node.tag, overflowInsets});
    node.mountedContentInsets = contentInsets;
    node.mountedOverflowInsets = overflowInsets;
    node.hasMountedInsets = true;
  }

#if defined(__APPLE__)
  // iOS paragraph views lay their text out in the state's content size, so a
  // resize must republish it even when the text itself is unchanged.
  if (node.textRoot && (changed || insetsChanged) &&
      node.hasTextMeasurementCache) {
    node.needsState = true;
    stateUpdates_.push_back(node.tag);
  }
#endif

  if (!changed) {
    return;
  }

  pendingOperations_.append(DirectLayoutOperation{
      node.tag,
      parent.tag,
      displayType,
      direction,
      nativeLeft,
      nativeTop,
      width,
      height});

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
    const std::function<const std::string*(Tag)>& componentNameFor,
    const std::function<const std::vector<Tag>* (Tag)>& childrenFor) {
  // The name lists live in LoomRendererInternals.h so a new alias is added
  // once; these kept their local names because the body below reads better with
  // them.
  const auto& isParagraph = isParagraphName;
  const auto& isVirtualText = isVirtualTextName;
  const auto& isRawText = isRawTextName;

  std::unordered_set<Tag> visited;
  std::function<Tag(Tag, bool)> visit;
  visit = [&](Tag tag, bool insideText) -> Tag {
    if (!visited.insert(tag).second) {
      return 0;
    }
    // A tag this commit does not know is treated exactly as before: no name and
    // no children, so it cannot be an unsupported attachment.
    const auto emptyName = std::string{};
    const auto* namePointer = componentNameFor(tag);
    const auto& name = namePointer == nullptr ? emptyName : *namePointer;
    const bool paragraph = isParagraph(name);
    const bool textInput = name.find("TextInput") != std::string::npos;
    const bool text = paragraph || isVirtualText(name);
    if (insideText && !text && !isRawText(name)) {
      return tag;
    }
    if (isRawText(name)) {
      return 0;
    }
    const auto* children = childrenFor(tag);
    if (children == nullptr) {
      return 0;
    }
    for (const auto childTag : *children) {
      if (const auto unsupported =
              visit(childTag, insideText || paragraph || textInput);
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

void LoomRenderer::emitDelete(Tag tag) {
  pendingOperations_.append(DirectDeleteOperation{tag});
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

void LoomRenderer::collectUnreachable() {
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
      !LoomStartupOptions::earlyCreate.load(std::memory_order_relaxed)) {
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
      !LoomStartupOptions::parallelText.load(std::memory_order_relaxed)) {
    return;
  }
#if !defined(__APPLE__)
  if (!LoomStartupOptions::parallelFor) {
    return;
  }
#endif
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
#if defined(__APPLE__)
    struct Context {
      const LoomRenderer* renderer;
      const std::vector<SpeculativeTextRequest>* requests;
      std::vector<TextMeasurement>* results;
    } context{this, &unique, &results};
    dispatch_apply_f(
        unique.size() - 1,
        dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
        &context,
        [](void* raw, size_t index) {
          auto* context = static_cast<Context*>(raw);
          const auto& request = (*context->requests)[index + 1];
          (*context->results)[index + 1] =
              context->renderer->measureTextWithPlatform(
                  *request.node, request.minimumSize, request.maximumSize);
        });
#else
    LoomStartupOptions::parallelFor(unique.size() - 1, [&](size_t index) {
      const auto& request = unique[index + 1];
      results[index + 1] = measureTextWithPlatform(
          *request.node, request.minimumSize, request.maximumSize);
    });
#endif
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
    // Contents are dead here; release capacity a big commit left behind. No
    // eager reserve: the vectors keep their capacity between commits, and
    // reserving the whole tree would defeat the trim for small commits.
    trimScratch(commitTraversal_);
    trimScratch(commitNodes_);
    trimScratch(commitLayoutParents_);
    commitTraversal_.clear();
    commitNodes_.clear();
    commitLayoutParents_.clear();
    prepare(
        *root,
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

    // Edges owned by removed parents were captured from the previous live tree.
    // Emit descendant edges first, before removing an ancestor or inserting a
    // moved child elsewhere.
    for (const auto& edge : removedMountedEdges_) {
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
      if (node->componentName != "RCTScrollView" ||
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
    releaseSlack();
    // Keep captured edges alive through collectUnreachable(): dropping a
    // removed parent walks its old children too, but those edges were already
    // emitted above. dropSubtree uses this list to avoid duplicate native
    // removes.
    removedMountedEdges_.clear();
    removedMountedEdgeKeys_.clear();
    removedNodeTags_.clear();
    removedNodeTagSet_.clear();
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
#if LOOM_PROFILE && defined(__ANDROID__)
    const auto profileMs = [](auto from, auto to) {
      return std::chrono::duration<double, std::milli>(to - from).count();
    };
    __android_log_print(
        ANDROID_LOG_INFO,
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
#if defined(__APPLE__)
      if (!preparedState.valid && node->textRoot && node->yogaNode != nullptr) {
        // Hosts without a prepared layout (iOS) still need the final content
        // box: it is the size the paragraph view lays its text out in.
        preparedState.size = Size{
            std::max(0.0f, YGNodeLayoutGetRawWidth(node->yogaNode) -
                               node->mountedContentInsets[0] -
                               node->mountedContentInsets[2]),
            std::max(0.0f, YGNodeLayoutGetRawHeight(node->yogaNode) -
                               node->mountedContentInsets[1] -
                               node->mountedContentInsets[3])};
      }
#endif
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
  stateUpdates_.clear();
  // Only nodes this commit's prepare walk reached can carry these marks: every
  // write that sets one also marks the node for that walk. A full commit
  // reached every live node, so this covers what the whole-map loop did.
  for (auto* node : commitNodes_) {
    node->createdThisCommit = false;
    node->replacesHostTagThisCommit = false;
    node->childrenDirty = false;
  }

  // Batch, text state, and layout notifications form one synchronous UI-thread
  // commit boundary. No observable layout callback escapes before all three
  // pieces are installed.
  emitLayoutEvents();
}

#pragma mark - Public API

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
#if LOOM_PROFILE && defined(__ANDROID__)
  // TEMPORARY DIAGNOSTIC: what viewport did the host actually hand us?
  __android_log_print(
      ANDROID_LOG_INFO,
      "LoomConstraints",
      "surface=%d minW=%.1f minH=%.1f maxW=%.1f maxH=%.1f rtl=%d",
      static_cast<int>(surfaceId_),
      minWidth_,
      minHeight_,
      maxWidth_,
      maxHeight_,
      isRTL_ ? 1 : 0);
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

void LoomRenderer::createNode(
    Tag tag,
    const std::string& componentName,
    const folly::dynamic& props,
    InstanceHandle::Shared instanceHandle) {
  if (isRawTextComponentName(componentName)) {
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

  node->componentName = hostComponentName(componentName);
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
      !isParagraphName(node.componentName) &&
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
        isParagraphName(node->componentName) ||
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
