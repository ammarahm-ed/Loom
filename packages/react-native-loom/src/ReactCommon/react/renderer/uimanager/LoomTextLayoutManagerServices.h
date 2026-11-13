/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <memory>

#include <react/renderer/componentregistry/componentNameByReactViewName.h>
#include <react/renderer/components/text/ParagraphProps.h>
#include <react/renderer/components/text/TextProps.h>
#include <react/renderer/core/LayoutConstraints.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include <react/renderer/core/RawPropsParser.h>
#include <react/renderer/textlayoutmanager/TextLayoutManager.h>
#include <react/renderer/uimanager/LoomTextServices.h>
#include <react/utils/ContextContainer.h>

namespace facebook::react {

/**
 * `LoomTextServices` for a host that has React Native's `TextLayoutManager`
 * (any of `textlayoutmanager/platform/{ios,android,cxx}`) and nothing else.
 *
 *   - `measureText` is `TextLayoutManager::measure`, with the point scale
 *     factor the host last gave `setPointScaleFactor`.
 *   - `parseTextAttributes` is React Native's own props classes: a Paragraph's
 *     props are parsed as `ParagraphProps`, a nested Text's as `TextProps`
 *     (`BaseTextProps` converts the raw props into `TextAttributes`), applied
 *     on top of the inherited attributes. This is exactly what the shadow-tree
 *     path builds, because the React name goes through the same
 *     `componentNameByReactViewName` the component registry uses. Raw text and
 *     every other component carry no attributes of their own.
 *   - No prepared layout.
 *   - Off-UI measurement is a constructor option: `TextLayoutManager::measure`
 *     is safe on any thread for the iOS and cxx implementations, which
 *     synchronize their caches; an Android-style manager that calls into the
 *     UI runtime is not.
 *
 * This header pulls in the text component and text layout modules, so it is
 * included by hosts, never by the core.
 */
class LoomTextLayoutManagerServices : public LoomTextServices {
 public:
  LoomTextLayoutManagerServices(
      std::shared_ptr<const ContextContainer> contextContainer,
      std::shared_ptr<const TextLayoutManager> layoutManager,
      bool measurableOnWorker)
      : contextContainer_(std::move(contextContainer)),
        layoutManager_(std::move(layoutManager)),
        measurableOnWorker_(measurableOnWorker) {}

  /** Call with the value given to `LoomBinding::setPointScaleFactor`. */
  void setPointScaleFactor(float pointScaleFactor) {
    pointScaleFactor_.store(pointScaleFactor, std::memory_order_relaxed);
  }

  std::optional<Size> measureText(
      SurfaceId surfaceId,
      const AttributedString& attributedString,
      const ParagraphAttributes& paragraphAttributes,
      const Size& minimumSize,
      const Size& maximumSize) override {
    TextLayoutContext layoutContext;
    layoutContext.surfaceId = surfaceId;
    layoutContext.pointScaleFactor =
        pointScaleFactor_.load(std::memory_order_relaxed);
    LayoutConstraints constraints;
    constraints.minimumSize = minimumSize;
    constraints.maximumSize = maximumSize;
    return layoutManager_
        ->measure(
            AttributedStringBox{attributedString},
            paragraphAttributes,
            layoutContext,
            constraints)
        .size;
  }

  bool canMeasureOnWorker() override {
    return measurableOnWorker_;
  }

  TextAttributes parseTextAttributes(
      SurfaceId surfaceId,
      const std::string& componentName,
      const folly::dynamic& props,
      const TextAttributes& inherited,
      ParagraphAttributes& outParagraphAttributes) override {
    auto attributes = inherited;
    const auto unifiedName = componentNameByReactViewName(componentName);
    if (unifiedName != "Paragraph" && unifiedName != "Text") {
      return attributes;
    }
    PropsParserContext context{surfaceId, *contextContainer_};
    RawProps rawProps{props};
    if (unifiedName == "Paragraph") {
      rawProps.parse(parser<ParagraphProps>());
      ParagraphProps parsed{context, ParagraphProps{}, rawProps};
      attributes.apply(parsed.textAttributes);
      outParagraphAttributes = parsed.paragraphAttributes;
    } else {
      rawProps.parse(parser<TextProps>());
      TextProps parsed{context, TextProps{}, rawProps};
      attributes.apply(parsed.textAttributes);
    }
    return attributes;
  }

 private:
  template <typename PropsT>
  static const RawPropsParser& parser() {
    struct Prepared {
      RawPropsParser parser;
      Prepared() {
        parser.prepare<PropsT>();
      }
    };
    static const Prepared prepared;
    return prepared.parser;
  }

  std::shared_ptr<const ContextContainer> contextContainer_;
  std::shared_ptr<const TextLayoutManager> layoutManager_;
  const bool measurableOnWorker_;
  std::atomic<float> pointScaleFactor_{1.0f};
};

} // namespace facebook::react
