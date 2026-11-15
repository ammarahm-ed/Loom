/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>

#include <folly/dynamic.h>
#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/attributedstring/TextAttributes.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/graphics/Size.h>
#include <react/renderer/loom/text/LoomTextMeasurement.h>

namespace facebook::react {

/**
 * What the core asks of a host's text engine. A host returns one from
 * `LoomHost::textServices()`; the core resolves it once and calls it directly.
 *
 * The minimum is `measureText`. A host that has React Native's
 * `TextLayoutManager` and nothing else uses `LoomTextLayoutManagerServices`
 * (LoomTextLayoutManagerServices.h), which implements the minimum and the
 * default attribute parser on top of it. Everything else is an optional
 * capability with a default that means "not supported":
 *
 *   - `parseTextAttributes`: a host with its own props parser. The default
 *     returns the inherited attributes unchanged, so a host that does not use
 *     `LoomTextLayoutManagerServices` should override it.
 *   - `prepareText` / `prepareTextForMount`: a reusable host layout. Set
 *     `LoomPlatformTraits::hasPreparedTextLayout` when `prepareTextForMount`
 *     returns one the host draws from.
 *   - `canMeasureOnWorker`: measurement off the UI thread.
 *
 * Ownership: the host owns the object for its own lifetime; the core never
 * deletes it. Every method is called with the surface's `SurfaceId`, and none
 * may block on the UI thread.
 */
class LoomTextServices {
 public:
  virtual ~LoomTextServices() noexcept = default;

  /**
   * Measures a resolved text payload within `[minimumSize, maximumSize]`, in
   * DIP. `std::nullopt` means the host could not measure and the core falls
   * back to an estimate. Called once per text node whose measure Yoga asks
   * for: on the commit thread when `canMeasureOnWorker()` is true, otherwise
   * the commit is computed on the UI thread.
   */
  virtual std::optional<Size> measureText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) = 0;

  /**
   * Whether `measureText` and `prepareText` may be called off the UI thread.
   * Called on the UI thread, only when the answer can change a decision, and
   * never cached across commits, so it may change at runtime.
   */
  virtual bool canMeasureOnWorker() {
    return false;
  }

  /**
   * Resolves a node's text props on top of the enclosing text's `inherited`
   * attributes. `outParagraphAttributes` is filled only for paragraph
   * components. `componentName` is the React name the node was created with.
   * Called on the commit thread, once per text node with changed props.
   */
  virtual TextAttributes parseTextAttributes(
      SurfaceId,
      const std::string& componentName,
      const folly::dynamic& props,
      const TextAttributes& inherited,
      ParagraphAttributes& outParagraphAttributes) {
    return inherited;
  }

  /**
   * Like `measureText`, and also returns a reusable host layout in
   * `LoomTextMeasurement::preparedLayout`. `valid == false` means
   * unsupported. The core fills in the measurement's constraints.
   */
  virtual LoomTextMeasurement prepareText(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) {
    return {};
  }

  /**
   * Builds the visual text state for mounting without measuring it for
   * layout. Same threading as `prepareText`.
   */
  virtual LoomTextMeasurement prepareTextForMount(
      SurfaceId,
      const AttributedString&,
      const ParagraphAttributes&,
      const Size& minimumSize,
      const Size& maximumSize) {
    return {};
  }
};

} // namespace facebook::react
