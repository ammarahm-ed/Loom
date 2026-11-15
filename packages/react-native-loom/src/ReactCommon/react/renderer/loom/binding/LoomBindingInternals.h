/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Private to the LoomBinding*.cpp files: the shared includes, the host-call profiling
// macro, the binding lifetime gate and the JS argument helpers they all use.

#include <glog/logging.h>
#include <unistd.h>
#include <folly/json.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include "LoomApiInternal.h"
#include "LoomBinding.h"
#include "LoomFabricBridge.h"
#include "../mounting/LoomLayoutAnimations.h"
#include "../core/LoomRendererInternals.h"
#include "../host/LoomSettingsHost.h"
#include "../worker/LoomWorkerProfile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>
#include <react/renderer/runtimescheduler/JankReporter.h>
#include <react/renderer/runtimescheduler/RuntimeSchedulerFrameCallbackScope.h>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_set>

#include <jsi/JSIDynamic.h>


#if LOOM_RENDERER
namespace facebook::react {

#if LOOM_PROFILE
#define LOOM_HOST_CALL_SCOPE(name, ...) \
  LoomBinding::HostCallScope name(__VA_ARGS__)
#else
#define LOOM_HOST_CALL_SCOPE(name, ...)
#endif
struct LoomBinding::Lifetime {
  std::shared_mutex gate;
  LoomBinding* binding{nullptr};
};

namespace {

using loom::propsFromValue;
using loom::requireArguments;

template <typename Draft>
void classifyDraft(Draft& draft) {
  draft.roles =
      LoomPlatformTraits::current().components.classify(*draft.componentName);
}

/**
 * Whether a node's live child list differs from the list this commit selected.
 * Uses the borrowed accessor: the copying form allocated a vector per node on
 * every commit just to compare it.
 */
[[maybe_unused]]
bool childrenDifferFrom(
    const LoomRenderer& renderer,
    Tag tag,
    const std::vector<Tag>& expected) {
  const auto* live = renderer.childrenOfRef(tag);
  if (live == nullptr) {
    return !expected.empty();
  }
  return *live != expected;
}

/**
 * Guards the two maps below. Rendering runs on the JS thread, which is the UI
 * thread in this mode, and surface callbacks arrive there too, so this is
 * insurance rather than a real contention point. It is deliberately never held
 * while a renderer runs: a commit can call back into the host, which can call
 * back into this binding.
 */
std::mutex& registryMutex() {
  static std::mutex mutex;
  return mutex;
}

/**
 * Constraints are keyed by surface rather than held per binding, because a
 * surface is started before JavaScript - and therefore before the binding that
 * will own its renderer - exists. Android assigns each surface its root tag;
 * ReactRootViewTagGenerator is a process-wide synchronized counter, so these
 * IDs do not collide across React instances in this process. Keep that
 * uniqueness requirement explicit: SurfaceId is also the key used by the
 * renderer and Java mounting manager.
 */
[[maybe_unused]]
std::unordered_map<SurfaceId, LoomBinding::SurfaceConstraints>&
surfaceConstraints() {
  static std::
      unordered_map<SurfaceId, LoomBinding::SurfaceConstraints>
          constraints;
  return constraints;
}

/** Bindings that currently own renderers, so a static call can reach them. */
std::unordered_map<
    LoomBinding*,
    std::shared_ptr<LoomBinding::Lifetime>>&
liveBindings() {
  static std::unordered_map<
      LoomBinding*,
      std::shared_ptr<LoomBinding::Lifetime>>
      bindings;
  return bindings;
}

[[maybe_unused]]
std::vector<std::shared_ptr<LoomBinding::Lifetime>>
bindingSnapshot() {
  std::vector<std::shared_ptr<LoomBinding::Lifetime>> snapshot;
  std::lock_guard lock(registryMutex());
  snapshot.reserve(liveBindings().size());
  for (const auto& entry : liveBindings()) {
    snapshot.push_back(entry.second);
  }
  return snapshot;
}

float& directPointScaleFactorStorage() {
  static float factor = 1.0f;
  return factor;
}

[[maybe_unused]]
float directPointScaleFactorValue() {
  std::lock_guard lock(registryMutex());
  return directPointScaleFactorStorage();
}



[[maybe_unused]]
std::optional<std::string> rawTextFromValue(
    jsi::Runtime& runtime,
    const jsi::Value& value) {
  if (!value.isObject()) {
    return std::nullopt;
  }
  auto object = value.asObject(runtime);
  auto names = object.getPropertyNames(runtime);
  if (names.size(runtime) != 1) {
    return std::nullopt;
  }
  auto name = names.getValueAtIndex(runtime, 0);
  if (!name.isString() || name.getString(runtime).utf8(runtime) != "text") {
    return std::nullopt;
  }
  auto text = object.getProperty(runtime, "text");
  if (!text.isString()) {
    return std::nullopt;
  }
  return text.getString(runtime).utf8(runtime);
}

/** `Node.compareDocumentPosition` bit values, as the DOM API defines them. */
constexpr unsigned kDocumentPositionDisconnected = 0x01;

} // namespace

namespace {

/**
 * The renderer as a layout-animation driver sees it. Layout reads are only
 * answered inside `processBatch` (`inCommit`), where the renderer matches the
 * batch being mounted.
 */
class RendererGeometry final : public LoomLayoutAnimationGeometry {
 public:
  RendererGeometry(const LoomRenderer& renderer, bool inCommit)
      : renderer_(renderer), inCommit_(inCommit) {}

  std::optional<Rect> layoutFrameInSurface(Tag tag) const override {
    LoomRenderer::Metrics metrics;
    if (!inCommit_ ||
        !renderer_.metricsRelativeTo(
            tag, 0, metrics, /*includeTransform*/ false,
            /*includeScroll*/ false)) {
      return std::nullopt;
    }
    return Rect{{metrics.x, metrics.y}, {metrics.width, metrics.height}};
  }

  Tag parentOf(Tag tag) const override {
    if (!inCommit_) {
      return 0;
    }
    const auto info = renderer_.fabricNodeInfo(tag);
    return info.has_value() ? info->parent : 0;
  }

  size_t mountedChildCount(Tag tag) const override {
    return inCommit_ ? renderer_.mountedChildCount(tag) : 0;
  }

  const std::string* componentNameOf(Tag tag) const override {
    if (!inCommit_) {
      return nullptr;
    }
    const auto info = renderer_.fabricNodeInfo(tag);
    return info.has_value() ? info->componentName : nullptr;
  }

  const folly::dynamic* propsOf(Tag tag) const override {
    if (!inCommit_) {
      return nullptr;
    }
    const auto info = renderer_.fabricNodeInfo(tag);
    return info.has_value() ? info->props : nullptr;
  }

  std::optional<Point> nativeOriginInSurface(Tag tag) const override {
    const auto location = renderer_.viewLocationInSurface(tag);
    if (!location.has_value() || std::isnan(location->first) ||
        std::isnan(location->second)) {
      return std::nullopt;
    }
    return Point{location->first, location->second};
  }

 private:
  const LoomRenderer& renderer_;
  const bool inCommit_;
};

} // namespace

} // namespace facebook::react
#endif // LOOM_RENDERER
