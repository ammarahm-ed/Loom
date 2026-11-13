/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <cstdint>

#include <react/renderer/attributedstring/AttributedString.h>
#include <react/renderer/attributedstring/ParagraphAttributes.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/graphics/Size.h>

namespace facebook::react {

struct LoomTextMeasurementProfile {
  uint64_t spannableBuildNanos{0};
  uint64_t paintSetupNanos{0};
  uint64_t layoutCreateNanos{0};
  uint64_t isBoringNanos{0};
  uint64_t boringMakeNanos{0};
  uint64_t measurementResultNanos{0};
  int layoutKind{0};
  bool simplePlainTextFastPath{false};
};

/** A Loom renderer text measurement and, when available, its reusable host layout. */
struct LoomTextMeasurement {
  Size size{};
  Size minimumSize{};
  Size maximumSize{};
  float pointScaleFactor{1.0f};
  bool valid{false};
  std::shared_ptr<void> preparedLayout;
  LoomTextMeasurementProfile profile{};
};

/** What the host needs to know about the component a state update is for. */
enum class LoomTextStateKind : uint8_t {
  /** Exactly `RCTText`: Android reads only the prepared layout. */
  Text,
  /** `Paragraph` / `Text`: a paragraph state built from the string. */
  Paragraph,
  /** `RCTSelectableText` / `SelectableParagraph`: Android paragraph state only. */
  SelectableParagraph,
  /** `TextInput`: also reads `mostRecentEventCount`. */
  TextInput,
  /** Any other component with a generic state wrapper (no text payload). */
  Other,
};

/**
 * The state a text root (or a generic-state component) publishes to its host
 * view. The renderer fills it once per update; the string is shared, never
 * copied, so the update moves through the worker's deferred queue and the
 * scheduler for the cost of a pointer.
 */
struct LoomTextStateUpdate {
  SurfaceId surfaceId{0};
  Tag tag{0};
  LoomTextStateKind kind{LoomTextStateKind::Other};
  /** Null for `Other`. */
  std::shared_ptr<const AttributedString> attributedString;
  ParagraphAttributes paragraphAttributes;
  LoomTextMeasurement prepared;
  /** `TextInput` only: `mostRecentEventCount` from its props. */
  int64_t mostRecentEventCount{0};
};

} // namespace facebook::react
