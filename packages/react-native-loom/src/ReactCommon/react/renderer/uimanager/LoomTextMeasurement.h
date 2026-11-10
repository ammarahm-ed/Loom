/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <cstdint>

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

} // namespace facebook::react
