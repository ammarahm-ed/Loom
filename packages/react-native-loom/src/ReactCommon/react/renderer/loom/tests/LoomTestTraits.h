/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file in the root directory.
 */

#pragma once

#include <functional>
#include <string>
#include <thread>

#include <react/renderer/loom/host/LoomPlatformTraits.h>
#include "LoomTestMode.h"
#include <react/renderer/loom/host/LoomViewManagerNames.h>

namespace facebook::react {

/**
 * Installs the traits the renderer tests run against: view-manager component
 * names, prop diffs for everything but Image, text state republished on
 * resize, no prepared text layout, and the thread that loads the test binary
 * as the UI thread. Initialized before `main`, on that thread.
 */
inline const bool kLoomTestTraitsInstalled = [] {
  static const auto uiThread = std::this_thread::get_id();
  LoomPlatformTraits traits;
  traits.components = loomViewManagerComponentTable();
  traits.sendsPropDiffs = [](const std::string& hostName) {
    return hostName.find("Image") == std::string::npos;
  };
  traits.republishTextStateOnResize = true;
  traits.isUIThread = [] { return std::this_thread::get_id() == uiThread; };
  traits.parallelFor = [](size_t count, const std::function<void(size_t)>& body) {
    for (size_t index = 0; index < count; ++index) {
      body(index);
    }
  };
  LoomPlatformTraits::install(std::move(traits));
  return true;
}();

} // namespace facebook::react
