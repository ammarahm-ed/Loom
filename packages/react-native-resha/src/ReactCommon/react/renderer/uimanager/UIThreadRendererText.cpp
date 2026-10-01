/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UIThreadRenderer.h"
#include "UIThreadRendererInternals.h"

#include <algorithm>
#include <cmath>
#include <limits>

#if UI_THREAD_RENDERER
namespace facebook::react {
namespace {

using uiThreadRenderer::findProp;
using uiThreadRenderer::numberProp;
using uiThreadRenderer::propsFromValue;
using uiThreadRenderer::requireArguments;
using uiThreadRenderer::stringProp;


TextAttributes textMeasurementAttributes(TextAttributes attributes) {
  // These affect drawing, not glyph metrics or line breaking. Keep all
  // unlisted attributes in the key so new/custom text behavior is conservative.
  attributes.foregroundColor = SharedColor{};
  attributes.backgroundColor = SharedColor{};
  attributes.opacity = std::numeric_limits<Float>::quiet_NaN();
  attributes.textDecorationColor = SharedColor{};
  attributes.textDecorationLineType.reset();
  attributes.textDecorationStyle.reset();
  attributes.textShadowOffset.reset();
  attributes.textShadowRadius = std::numeric_limits<Float>::quiet_NaN();
  attributes.textShadowColor = SharedColor{};
  return attributes;
}

bool isTextInputName(const std::string& componentName) {
  return componentName.find("TextInput") != std::string::npos;
}

UIThreadRenderer::TextMeasurement estimateTextMeasurement(
    const TextAttributes& attributes,
    const AttributedString& attributedString,
    float maxWidth,
    bool widthBounded) {
  const auto text = attributedString.getString();
  const float fontSize =
      std::isnan(attributes.fontSize) || attributes.fontSize <= 0
      ? 14.0f
      : attributes.fontSize;
  const float lineHeight =
      std::isnan(attributes.lineHeight) || attributes.lineHeight <= 0
      ? fontSize * 1.2f
      : attributes.lineHeight;
  // Roboto's average advance for mixed-case Latin text sits near 0.5em.
  const float charWidth = fontSize * 0.55f;

  const float available = widthBounded && maxWidth > 0
      ? maxWidth
      : std::numeric_limits<float>::max();
  float widest = 0;
  float lineWidth = 0;
  float wordWidth = 0;
  int lines = 1;
  for (size_t index = 0; index <= text.size(); ++index) {
    const bool atBreak =
        index == text.size() || text[index] == ' ' || text[index] == '\n';
    if (!atBreak) {
      wordWidth += charWidth;
      continue;
    }
    if (lineWidth > 0 && lineWidth + wordWidth > available) {
      widest = std::max(widest, lineWidth);
      lines += 1;
      lineWidth = wordWidth;
    } else {
      lineWidth += wordWidth;
    }
    wordWidth = 0;
    if (index != text.size() && text[index] == '\n') {
      widest = std::max(widest, lineWidth);
      lines += 1;
      lineWidth = 0;
    } else if (index != text.size()) {
      lineWidth += charWidth;
    }
  }
  widest = std::max(widest, lineWidth);

  UIThreadRenderer::TextMeasurement measurement;
  measurement.valid = true;
  measurement.width = widthBounded ? std::min(widest, available) : widest;
  measurement.height = lineHeight * static_cast<float>(lines);
  return measurement;
}

} // namespace

bool UIThreadRenderer::sameTextMeasurementContent(
    const AttributedString& left,
    const AttributedString& right) {
  if (textMeasurementAttributes(left.getBaseTextAttributes()) !=
      textMeasurementAttributes(right.getBaseTextAttributes())) {
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
    if (leftFragment.string != rightFragment.string ||
        textMeasurementAttributes(leftFragment.textAttributes) !=
            textMeasurementAttributes(rightFragment.textAttributes) ||
        leftFragment.parentShadowView.tag !=
            rightFragment.parentShadowView.tag) {
      return false;
    }
  }
  return true;
}

#pragma mark - Text

/**
 * A text root's content is rebuilt from its subtree whenever any node under it
 * changes. Android renders text exclusively from the paragraph state, so this
 * attributed string - not the props - is what decides how the text looks.
 */
void UIThreadRenderer::markTextDirty(Tag tag) {
  for (Tag current = tag; current != 0;) {
    if (const auto* leaf = findRawText(current); leaf != nullptr) {
      current = leaf->parent;
      continue;
    }
    auto* node = find(current);
    if (node == nullptr) {
      return;
    }
    if (node->textRoot) {
      node->textDirty = true;
      return;
    }
    if (!node->textLike) {
      return;
    }
    current = node->parent;
  }
}

TextAttributes UIThreadRenderer::attributesFor(
    Node& node,
    const TextAttributes& inherited) {
  const bool isParagraph = node.textRoot;
  auto& cached = node.parsedTextAttributes;
  // The parse depends only on these three, so a match means the previous answer
  // is still exact. That matters because a text root rebuilds its whole subtree
  // and each node in it would otherwise cross the bridge again, even the ones
  // whose props did not change.
  if (cached.valid && cached.propsGeneration == node.propsGeneration &&
      cached.textRoot == isParagraph && cached.inherited == inherited) {
    if (isParagraph) {
      node.paragraphAttributes = cached.paragraphAttributes;
    }
    return cached.attributes;
  }

  ParagraphAttributes paragraphAttributes;
  auto attributes = operations_.parseTextAttributes
      ? operations_.parseTextAttributes(
            surfaceId_,
            node.componentName,
            node.props,
            inherited,
            isParagraph ? &paragraphAttributes : nullptr)
      : inherited;
  if (isParagraph) {
    node.paragraphAttributes = std::move(paragraphAttributes);
  }
  cached.valid = true;
  cached.propsGeneration = node.propsGeneration;
  cached.textRoot = isParagraph;
  cached.inherited = inherited;
  cached.attributes = attributes;
  cached.paragraphAttributes = node.paragraphAttributes;
  return attributes;
}

void UIThreadRenderer::appendTextContent(
    Node& node,
    const TextAttributes& inherited,
    AttributedString& out) {
  bool lastFragmentWasRawText = false;
  for (const Tag childTag : node.children) {
    if (const auto* rawText = findRawText(childTag); rawText != nullptr) {
      if (lastFragmentWasRawText && !out.getFragments().empty()) {
        out.getFragments().back().string += rawText->text;
      } else {
        auto fragment = AttributedString::Fragment{};
        fragment.string = rawText->text;
        fragment.textAttributes = inherited;
        fragment.parentShadowView = shadowView(node.tag);
        out.appendFragment(std::move(fragment));
        lastFragmentWasRawText = true;
      }
      continue;
    }
    auto* child = find(childTag);
    if (child == nullptr) {
      continue;
    }

    lastFragmentWasRawText = false;

    if (child->textLike) {
      // Nested text applies its own attributes on top of what it inherits,
      // so that an unset attribute keeps the enclosing span's value.
      child->textAttributes = attributesFor(*child, inherited);
      appendTextContent(*child, child->textAttributes, out);
      continue;
    }

    // Anything else (a view nested in text) has no safe direct host contract:
    // Android paragraph views do not mount arbitrary child views inline.
    if (!child->inlineAttachmentReported &&
        operations_.reportUnsupportedInlineAttachment) {
      operations_.reportUnsupportedInlineAttachment(surfaceId_, child->tag);
      child->inlineAttachmentReported = true;
    }
    unsupportedInlineAttachment_ = true;
    return;
  }
}

void UIThreadRenderer::buildTextContent(Node& root) {
  auto textAttributes =
      attributesFor(root, TextAttributes::defaultTextAttributes());
  textAttributes.layoutDirection =
      isRTL_ ? LayoutDirection::RightToLeft : LayoutDirection::LeftToRight;
  root.textAttributes = textAttributes;

  auto attributedString = AttributedString{};
  const bool textInput = isTextInputName(root.componentName);
  const auto* controlledValue = findProp(root.props, "value");
  const auto* initialDefaultValue = findProp(root.props, "defaultValue");
  const bool publishTextState = !textInput || controlledValue != nullptr ||
      (!root.hostCreated && initialDefaultValue != nullptr);
  if (textInput) {
    // TextInput's displayed content is a host prop, not a React child. Reading
    // only its children publishes an empty AndroidTextInputState on every
    // rerender and erases the character just typed into a controlled input.
    const auto* value = controlledValue != nullptr
        ? controlledValue
        : (!root.hostCreated ? initialDefaultValue : nullptr);
    if (value != nullptr && value->isString() && !value->getString().empty()) {
      auto fragment = AttributedString::Fragment{};
      fragment.string = value->getString();
      fragment.textAttributes = textAttributes;
      fragment.parentShadowView = shadowView(root.tag);
      attributedString.appendFragment(std::move(fragment));
    }
  } else {
    appendTextContent(root, textAttributes, attributedString);
  }
  attributedString.setBaseTextAttributes(textAttributes);
  const bool measurementInputsChanged = !root.hasTextMeasurementCache ||
      !sameTextMeasurementContent(
          root.measuredAttributedString, attributedString) ||
      root.measuredParagraphAttributes != root.paragraphAttributes;
  root.attributedString = std::move(attributedString);
  // What Yoga measures. For an empty input with a hint this is the hint, which
  // is what keeps an auto-height `TextInput` from collapsing to zero height;
  // `attributedString` above stays the real content so the state we publish is
  // the user's text and the host draws its own placeholder. Mirrors Fabric's
  // `attributedStringBoxToMeasure` / `getPlaceholderAttributedString` split.
  root.measurementAttributedString = root.attributedString;
  if (textInput && root.measurementAttributedString.isEmpty()) {
    const auto* placeholder = findProp(root.props, "placeholder");
    if (placeholder != nullptr && placeholder->isString() &&
        !placeholder->getString().empty()) {
      auto fragment = AttributedString::Fragment{};
      fragment.string = placeholder->getString();
      fragment.textAttributes = root.measurementAttributedString
                                    .getBaseTextAttributes();
      fragment.parentShadowView = shadowView(root.tag);
      root.measurementAttributedString.appendFragment(std::move(fragment));
    }
  }
  root.textDirty = false;
  if (publishTextState) {
    root.needsState = true;
    stateUpdates_.push_back(root.tag);
  }
  if (measurementInputsChanged && root.yogaNode != nullptr) {
    YGNodeMarkDirty(root.yogaNode);
  }
}

UIThreadRenderer::TextMeasurement UIThreadRenderer::measureTextNode(
    const Node& node,
    const Size& minimumSize,
    const Size& maximumSize) const {
  // Content and attributes must match before any entry below can be trusted;
  // they are shared by every constraint pair measured for this text.
  const bool cacheableContent = node.hasTextMeasurementCache &&
      sameTextMeasurementContent(
          node.measuredAttributedString, node.measurementAttributedString) &&
      node.measuredParagraphAttributes == node.paragraphAttributes;
  if (cacheableContent) {
    for (size_t index = 0; index < node.recentTextMeasurementCount_; ++index) {
      const auto& entry = node.recentTextMeasurements_[index];
      if (entry.minimumSize == minimumSize &&
          entry.maximumSize == maximumSize &&
          std::abs(entry.pointScaleFactor - pointScaleFactor_) <= 0.0001f) {
#if RN_PROFILE_UI_THREAD_RENDERER
        ++profileTextMeasureCacheHits_;
#endif
        return entry.measurement;
      }
    }
  }
  if (cacheableContent &&
      node.measuredMinimumSize == minimumSize &&
      node.measuredMaximumSize == maximumSize &&
      std::abs(node.measuredPointScaleFactor - pointScaleFactor_) <= 0.0001f) {
#if RN_PROFILE_UI_THREAD_RENDERER
    ++profileTextMeasureCacheHits_;
#endif
    return node.measuredText;
  }
  if (operations_.prepareText || operations_.measureText) {
#if RN_PROFILE_UI_THREAD_RENDERER
    const auto measureStart = std::chrono::steady_clock::now();
#endif
    TextMeasurement measurement;
    if (operations_.prepareText) {
      measurement.prepared = operations_.prepareText(
          surfaceId_,
          node.measurementAttributedString,
          node.paragraphAttributes,
          minimumSize,
          maximumSize);
      if (measurement.prepared.valid) {
#if RN_PROFILE_UI_THREAD_RENDERER
        const auto& profile = measurement.prepared.profile;
        profileTextSpannableBuildNanos_ += profile.spannableBuildNanos;
        profileTextPaintSetupNanos_ += profile.paintSetupNanos;
        profileTextLayoutCreateNanos_ += profile.layoutCreateNanos;
        profileTextIsBoringNanos_ += profile.isBoringNanos;
        profileTextBoringMakeNanos_ += profile.boringMakeNanos;
        profileTextMeasurementResultNanos_ += profile.measurementResultNanos;
        profileSimplePlainTextLayouts_ += profile.simplePlainTextFastPath ? 1 : 0;
        profileTextBoringLayouts_ += profile.layoutKind == 1 ? 1 : 0;
        profileTextStaticLayouts_ += profile.layoutKind == 2 ? 1 : 0;
#endif
        measurement.width = measurement.prepared.size.width;
        measurement.height = measurement.prepared.size.height;
        measurement.valid = true;
      }
    }
    if (!measurement.valid && operations_.measureText) {
      measurement = operations_.measureText(
          surfaceId_,
          node.measurementAttributedString,
          node.paragraphAttributes,
          minimumSize,
          maximumSize);
    }
#if RN_PROFILE_UI_THREAD_RENDERER
    profileTextMeasureNanoseconds_ += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - measureStart)
            .count());
    ++profileTextMeasureCalls_;
#endif
    if (measurement.valid) {
      node.measuredAttributedString = node.measurementAttributedString;
      node.measuredParagraphAttributes = node.paragraphAttributes;
      node.measuredMinimumSize = minimumSize;
      node.measuredMaximumSize = maximumSize;
      node.measuredPointScaleFactor = pointScaleFactor_;
      node.measuredText = measurement;
      node.hasTextMeasurementCache = true;
      node.preparedAttributedString = node.measurementAttributedString;
      node.preparedParagraphAttributes = node.paragraphAttributes;
      // Remember this constraint pair too, so the next pass that asks for it
      // again does not rebuild the platform layout.
      const auto slot = node.recentTextMeasurementCount_ <
              Node::kTextMeasurementCacheEntries
          ? node.recentTextMeasurementCount_++
          : Node::kTextMeasurementCacheEntries - 1;
      auto& entry = node.recentTextMeasurements_[slot];
      entry.minimumSize = minimumSize;
      entry.maximumSize = maximumSize;
      entry.pointScaleFactor = pointScaleFactor_;
      entry.measurement = measurement;
      return measurement;
    }
  }
  const bool bounded =
      std::isfinite(maximumSize.width) && maximumSize.width > 0;
  auto estimate = estimateTextMeasurement(
      node.textAttributes,
      node.measurementAttributedString,
      maximumSize.width,
      bounded);
  if (!node.measurementAttributedString.getString().empty() &&
      estimate.height <= 0) {
    const auto fontSize = std::isnan(node.textAttributes.fontSize) ||
            node.textAttributes.fontSize <= 0
        ? 14.0f
        : node.textAttributes.fontSize;
    estimate.height = fontSize * 1.2f;
  }
  return estimate;
}

YGSize UIThreadRenderer::measureText(
    YGNodeConstRef yogaNode,
    float width,
    YGMeasureMode widthMode,
    float height,
    YGMeasureMode heightMode) {
  auto* node = static_cast<Node*>(YGNodeGetContext(yogaNode));
  if (node == nullptr || node->owner == nullptr) {
    return YGSize{0, 0};
  }

  // Mirror `YogaLayoutableShadowNode`'s measurement connector: an exact mode
  // is a min == max clamp, "at most" only bounds the maximum, and an
  // undefined mode leaves the axis unbounded.
  auto minimumSize = Size{0, 0};
  auto maximumSize = Size{
      std::numeric_limits<Float>::infinity(),
      std::numeric_limits<Float>::infinity()};
  if (widthMode == YGMeasureModeExactly) {
    minimumSize.width = width;
    maximumSize.width = width;
  } else if (widthMode == YGMeasureModeAtMost) {
    maximumSize.width = width;
  }
  if (heightMode == YGMeasureModeExactly) {
    minimumSize.height = height;
    maximumSize.height = height;
  } else if (heightMode == YGMeasureModeAtMost) {
    maximumSize.height = height;
  }

  auto measurement =
      node->owner->measureTextNode(*node, minimumSize, maximumSize);
  if (!measurement.valid) {
    return YGSize{0, 0};
  }
  return YGSize{
      std::max(0.0f, measurement.width), std::max(0.0f, measurement.height)};
}

} // namespace facebook::react
#endif // UI_THREAD_RENDERER
