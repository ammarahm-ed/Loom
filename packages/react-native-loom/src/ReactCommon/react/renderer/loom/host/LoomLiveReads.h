/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include <optional>
#include <utility>
#include <vector>

namespace facebook::react {

/**
 * Live native state read together. Reading it is a trip to the UI thread for a
 * caller on any other thread, so a measurement asks for everything it needs at
 * once.
 */
struct LoomLiveReads {
  /** One scroll position per requested tag, in the order requested (DIP). */
  std::vector<std::pair<float, float>> scrollOffsets;
  /**
   * The requested view's origin in its surface (DIP), when one was requested
   * and the view is mounted.
   */
  std::optional<std::pair<float, float>> location;
};

} // namespace facebook::react
