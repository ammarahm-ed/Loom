/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The library API (react/loom/Loom.h) on top of Loom's renderer: reads and prop writes go through
// LoomBinding's `api*` entry points; frame and commit notifications are fed by LoomBinding.cpp.

#include "../../../loom/Loom.h"

#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/loom/binding/LoomBinding.h>
#include <react/renderer/loom/host/LoomHost.h>
#include <react/renderer/uimanager/UIManagerBinding.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "LoomApiInternal.h"

namespace facebook::react::loom {

namespace internal {
namespace {

struct Listeners {
  std::mutex mutex;
  uint64_t next{1};
  std::unordered_map<uint64_t, Runtime::CommitListener> commit;
  std::unordered_map<uint64_t, Runtime::FrameListener> frame;
  std::unordered_map<SurfaceId, std::unordered_set<Tag>> changedSinceFrame;
  std::unordered_map<SurfaceId, std::vector<std::function<void()>>> beforeFrame;
  std::atomic<int> commitCount{0};
  std::atomic<int> frameCount{0};
  std::atomic<int> beforeCount{0};
};

Listeners& listeners() {
  static Listeners* instance = new Listeners(); // outlives every thread that may still call in
  return *instance;
}

} // namespace

bool wantsLayoutCommitted() {
  auto& l = listeners();
  return l.commitCount.load(std::memory_order_relaxed) > 0 ||
      l.frameCount.load(std::memory_order_relaxed) > 0;
}

void notifyLayoutCommitted(SurfaceId surfaceId, std::vector<Tag> changed) {
  auto& l = listeners();
  std::vector<Runtime::CommitListener> targets;
  {
    std::lock_guard lock(l.mutex);
    if (l.frameCount.load(std::memory_order_relaxed) > 0) {
      l.changedSinceFrame[surfaceId].insert(changed.begin(), changed.end());
    }
    for (const auto& entry : l.commit) {
      targets.push_back(entry.second);
    }
  }
  for (const auto& target : targets) {
    target(surfaceId, changed);
  }
}

void notifyFrameApplied(SurfaceId surfaceId) {
  auto& l = listeners();
  if (l.frameCount.load(std::memory_order_relaxed) == 0) {
    return;
  }
  FrameInfo info;
  info.surfaceId = surfaceId;
  info.frameTime = std::chrono::steady_clock::now();
  std::vector<Runtime::FrameListener> targets;
  {
    std::lock_guard lock(l.mutex);
    auto changed = l.changedSinceFrame.find(surfaceId);
    if (changed != l.changedSinceFrame.end()) {
      info.changed.assign(changed->second.begin(), changed->second.end());
      l.changedSinceFrame.erase(changed);
    }
    for (const auto& entry : l.frame) {
      targets.push_back(entry.second);
    }
  }
  for (const auto& target : targets) {
    target(info);
  }
}

void runBeforeFrame() {
  auto& l = listeners();
  if (l.beforeCount.load(std::memory_order_relaxed) == 0) {
    return;
  }
  std::vector<std::function<void()>> due;
  {
    std::lock_guard lock(l.mutex);
    for (auto& entry : l.beforeFrame) {
      for (auto& callback : entry.second) {
        due.push_back(std::move(callback));
      }
    }
    l.beforeFrame.clear();
    l.beforeCount.store(0, std::memory_order_relaxed);
  }
  for (const auto& callback : due) {
    callback();
  }
}

} // namespace internal

namespace {

bool isLayoutProp(PropName name) {
  switch (name) {
    case WIDTH:
    case HEIGHT:
    case BORDER_WIDTH:
    case MARGIN:
    case PADDING:
    case POSITION:
    case FLEX:
    case ALIGN_CONTENT:
    case ALIGN_ITEMS:
    case ALIGN_SELF:
    case ASPECT_RATIO:
    case BOX_SIZING:
    case DISPLAY:
    case FLEX_BASIS:
    case FLEX_DIRECTION:
    case ROW_GAP:
    case COLUMN_GAP:
    case FLEX_GROW:
    case FLEX_SHRINK:
    case FLEX_WRAP:
    case JUSTIFY_CONTENT:
    case MAX_HEIGHT:
    case MAX_WIDTH:
    case MIN_HEIGHT:
    case MIN_WIDTH:
    case STYLE_OVERFLOW:
    case POSITION_TYPE:
    case DIRECTION:
      return true;
    default:
      return false;
  }
}

LayoutMetrics toLayoutMetrics(const LoomRenderer::Metrics& metrics) {
  LayoutMetrics layoutMetrics;
  layoutMetrics.frame.origin = {metrics.x, metrics.y};
  layoutMetrics.frame.size = {metrics.width, metrics.height};
  return layoutMetrics;
}

} // namespace

std::optional<Runtime> Runtime::from(jsi::Runtime& runtime) {
  auto binding = UIManagerBinding::getBinding(runtime);
  if (binding == nullptr || dynamic_cast<LoomBinding*>(binding.get()) == nullptr) {
    return std::nullopt;
  }
  return Runtime();
}

std::optional<Runtime> Runtime::from(const ContextContainer& contextContainer) {
  if (LoomHost::findIn(contextContainer) == nullptr) {
    return std::nullopt;
  }
  return Runtime();
}

void Runtime::forEachSurface(const std::function<void(SurfaceId)>& visit) const {
  LoomBinding::apiForEachSurface(visit);
}

std::optional<Size> Runtime::surfaceSize(SurfaceId surfaceId) const {
  std::optional<Size> size;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer&, const LoomBinding::SurfaceConstraints& constraints) {
    size = Size{constraints.maxWidth, constraints.maxHeight};
  });
  return size;
}

bool Runtime::hasNode(SurfaceId surfaceId, Tag tag) const {
  bool found = false;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    found = renderer.exists(tag);
  });
  return found;
}

std::optional<folly::dynamic> Runtime::props(SurfaceId surfaceId, Tag tag) const {
  std::optional<folly::dynamic> result;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    if (auto info = renderer.fabricNodeInfo(tag); info.has_value() && info->props != nullptr) {
      result = *info->props;
    }
  });
  return result;
}

std::optional<LayoutMetrics> Runtime::layoutRelativeTo(SurfaceId surfaceId, Tag tag, std::optional<Tag> ancestor) const {
  std::optional<LayoutMetrics> result;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    LoomRenderer::Metrics metrics;
    if (renderer.metricsRelativeTo(tag, ancestor.value_or(0), metrics)) {
      result = toLayoutMetrics(metrics);
    }
  });
  return result;
}

std::optional<LayoutMetrics> Runtime::layout(SurfaceId surfaceId, Tag tag) const {
  std::optional<LayoutMetrics> result;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    LoomRenderer::Metrics metrics;
    const auto parentTag = renderer.parentOf(tag);
    if (parentTag != 0 && renderer.metricsRelativeTo(tag, parentTag, metrics, /*includeTransform*/ false, /*includeScroll*/ false)) {
      result = toLayoutMetrics(metrics);
    }
  });
  return result;
}

std::optional<std::string> Runtime::componentName(SurfaceId surfaceId, Tag tag) const {
  std::optional<std::string> result;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    if (renderer.exists(tag)) {
      result = renderer.componentName(tag);
    }
  });
  return result;
}

std::vector<Tag> Runtime::children(SurfaceId surfaceId, Tag tag) const {
  std::vector<Tag> result;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    result = renderer.childrenOf(tag);
  });
  return result;
}

std::optional<Tag> Runtime::parent(SurfaceId surfaceId, Tag tag) const {
  std::optional<Tag> result;
  LoomBinding::apiRead(surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    if (renderer.exists(tag)) {
      if (auto parentTag = renderer.parentOf(tag); parentTag != 0) {
        result = parentTag;
      }
    }
  });
  return result;
}

void Runtime::updateProps(SurfaceId surfaceId, Tag tag, folly::dynamic props) const {
  LoomBinding::apiUpdateProps(surfaceId, tag, std::move(props));
}

void Runtime::updateAnimatedProps(SurfaceId /*surfaceId*/, Tag tag, AnimatedPropBag props) const {
  bool layout = false;
  for (const auto& prop : props.props) {
    layout = layout || isLayoutProp(prop->propName);
  }
  // A layout prop is written by the lane itself (layout, then the view); a paint-only bag was
  // applied to the view by the platform's batched write, so the lane only records it.
  LoomBinding::recordAnimationProps(tag, animationbackend::packAnimatedProps(props), /*viewApplied=*/!layout);
}

void Runtime::updateNativeProps(Tag tag, folly::dynamic props) const {
  LoomBinding::recordNativeProps(tag, std::move(props));
}

Subscription Runtime::onFrameApplied(FrameListener listener) const {
  auto& l = internal::listeners();
  uint64_t id;
  {
    std::lock_guard lock(l.mutex);
    id = l.next++;
    l.frame.emplace(id, std::move(listener));
    l.frameCount.fetch_add(1, std::memory_order_relaxed);
  }
  return Subscription([id] {
    auto& l = internal::listeners();
    std::lock_guard lock(l.mutex);
    if (l.frame.erase(id) != 0) {
      l.frameCount.fetch_sub(1, std::memory_order_relaxed);
    }
  });
}

Subscription Runtime::onLayoutCommitted(CommitListener listener) const {
  auto& l = internal::listeners();
  uint64_t id;
  {
    std::lock_guard lock(l.mutex);
    id = l.next++;
    l.commit.emplace(id, std::move(listener));
    l.commitCount.fetch_add(1, std::memory_order_relaxed);
  }
  return Subscription([id] {
    auto& l = internal::listeners();
    std::lock_guard lock(l.mutex);
    if (l.commit.erase(id) != 0) {
      l.commitCount.fetch_sub(1, std::memory_order_relaxed);
    }
  });
}

void Runtime::beforeNextFrame(SurfaceId surfaceId, std::function<void()> callback) const {
  auto& l = internal::listeners();
  std::lock_guard lock(l.mutex);
  l.beforeFrame[surfaceId].push_back(std::move(callback));
  l.beforeCount.fetch_add(1, std::memory_order_relaxed);
}

bool Runtime::updateState(SurfaceId surfaceId, Tag tag, const StateUpdate& update) const {
  if (update.family == nullptr || update.family->getSurfaceId() != surfaceId ||
      update.family->getTag() != tag) {
    return false;
  }
  return LoomBinding::dispatchComponentState(update);
}

std::optional<int> Runtime::setting(LoomSettingId id) const {
  if (id >= LoomSettingId::Count) {
    return std::nullopt;
  }
  return LoomSettings::get(id);
}

} // namespace facebook::react::loom
