/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The library API (react/loom/Loom.h) on top of Loom's renderer: reads and prop writes go through
// LoomBinding's `api*` entry points; frame and commit notifications are fed by LoomBinding.cpp.

// Only the Loom renderer build has the library API; a loom.renderer=false build compiles this file empty.
#if LOOM_RENDERER

#include "../../../loom/Loom.h"

#include <cmath>

#include <react/renderer/animationbackend/AnimatedPropsSerializer.h>
#include <react/renderer/loom/binding/LoomBinding.h>
#include <react/renderer/loom/host/LoomHost.h>
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <glog/logging.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "LoomApiInternal.h"

namespace facebook::react::loom {

namespace internal {

// ---- Instance ---------------------------------------------------------------------------------------

std::shared_ptr<Instance> Instance::in(const ContextContainer& container) {
  if (auto found = container.find<std::shared_ptr<Instance>>(kContextKey)) {
    return *found;
  }
  // `insert` keeps an existing entry, so two threads racing here end up with the same instance.
  container.insert(kContextKey, std::make_shared<Instance>());
  return container.at<std::shared_ptr<Instance>>(kContextKey);
}

void Instance::attach(std::weak_ptr<LoomBinding::Lifetime> lifetime) {
  std::lock_guard lock(bindingMutex_);
  lifetime_ = std::move(lifetime);
}

void Instance::detach(const LoomBinding::Lifetime* lifetime) {
  std::lock_guard lock(bindingMutex_);
  if (lifetime_.lock().get() == lifetime) {
    lifetime_.reset();
  }
}

bool Instance::withBinding(const std::function<void(LoomBinding&)>& body) const {
  std::shared_ptr<LoomBinding::Lifetime> lifetime;
  {
    std::lock_guard lock(bindingMutex_);
    lifetime = lifetime_.lock();
  }
  if (lifetime == nullptr) {
    return false;
  }
  std::shared_lock gate(lifetime->gate);
  if (lifetime->binding == nullptr) {
    return false;
  }
  body(*lifetime->binding);
  return true;
}

namespace {

bool libraryVersionSupported(int libraryApiVersion) {
  if (libraryApiVersion >= LOOM_API_MIN_SUPPORTED_VERSION && libraryApiVersion <= LOOM_API_VERSION) {
    return true;
  }
  static std::once_flag logged;
  std::call_once(logged, [libraryApiVersion] {
    LOG(ERROR) << "Loom: a library was compiled against Loom API " << libraryApiVersion
               << ", but this Loom provides API " << LOOM_API_MIN_SUPPORTED_VERSION << ".." << LOOM_API_VERSION
               << ". loom::Runtime::from returns nullopt for it; update the library.";
  });
  return false;
}

} // namespace

std::shared_ptr<Instance> instanceFrom(jsi::Runtime& runtime, int libraryApiVersion) {
  if (!libraryVersionSupported(libraryApiVersion)) {
    return nullptr;
  }
  auto binding = std::dynamic_pointer_cast<LoomBinding>(UIManagerBinding::getBinding(runtime));
  return binding == nullptr ? nullptr : binding->apiInstance();
}

std::shared_ptr<Instance> instanceFrom(const ContextContainer& contextContainer, int libraryApiVersion) {
  if (!libraryVersionSupported(libraryApiVersion) || LoomHost::findIn(contextContainer) == nullptr) {
    return nullptr;
  }
  return Instance::in(contextContainer);
}

// ---- Notifications from the renderer ------------------------------------------------------------------

bool wantsLayoutCommitted(const Instance& instance) {
  return instance.commitCount.load(std::memory_order_relaxed) > 0 ||
      instance.frameCount.load(std::memory_order_relaxed) > 0;
}

void notifyLayoutCommitted(Instance& instance, SurfaceId surfaceId, std::vector<Tag> changed) {
  std::vector<Runtime::CommitListener> targets;
  {
    std::lock_guard lock(instance.mutex);
    if (instance.frameCount.load(std::memory_order_relaxed) > 0) {
      instance.changedSinceFrame[surfaceId].insert(changed.begin(), changed.end());
    }
    targets.reserve(instance.commit.size());
    for (const auto& entry : instance.commit) {
      targets.push_back(entry.second);
    }
  }
  for (const auto& target : targets) {
    target(surfaceId, changed);
  }
}

void runBeforeFrame(Instance& instance, SurfaceId surfaceId) {
  if (instance.beforeCount.load(std::memory_order_relaxed) == 0) {
    return;
  }
  std::vector<std::function<void()>> due;
  {
    std::lock_guard lock(instance.mutex);
    auto entry = instance.beforeFrame.find(surfaceId);
    if (entry == instance.beforeFrame.end()) {
      return;
    }
    due = std::move(entry->second);
    instance.beforeFrame.erase(entry);
    instance.beforeCount.fetch_sub(static_cast<int>(due.size()), std::memory_order_relaxed);
  }
  for (const auto& callback : due) {
    callback();
  }
}

void notifyFrameApplied(Instance& instance, SurfaceId surfaceId) {
  if (instance.frameCount.load(std::memory_order_relaxed) == 0) {
    return;
  }
  FrameInfo info;
  info.surfaceId = surfaceId;
  info.frameTime = std::chrono::steady_clock::now();
  std::vector<Runtime::FrameListener> targets;
  {
    std::lock_guard lock(instance.mutex);
    if (auto changed = instance.changedSinceFrame.find(surfaceId); changed != instance.changedSinceFrame.end()) {
      info.changed.assign(changed->second.begin(), changed->second.end());
      instance.changedSinceFrame.erase(changed);
    }
    targets.reserve(instance.frame.size());
    for (const auto& entry : instance.frame) {
      targets.push_back(entry.second);
    }
  }
  for (const auto& target : targets) {
    target(info);
  }
}

void releaseSurface(Instance& instance, SurfaceId surfaceId) {
  std::lock_guard lock(instance.mutex);
  instance.changedSinceFrame.erase(surfaceId);
  if (auto entry = instance.beforeFrame.find(surfaceId); entry != instance.beforeFrame.end()) {
    instance.beforeCount.fetch_sub(static_cast<int>(entry->second.size()), std::memory_order_relaxed);
    instance.beforeFrame.erase(entry);
  }
}

} // namespace internal

namespace {

LayoutMetrics toLayoutMetrics(const LoomRenderer::Metrics& metrics) {
  LayoutMetrics layoutMetrics;
  layoutMetrics.frame.origin = {metrics.x, metrics.y};
  layoutMetrics.frame.size = {metrics.width, metrics.height};
  return layoutMetrics;
}

} // namespace

// ---- Reads ------------------------------------------------------------------------------------------

namespace {

/** Runs `read` against the surface's renderer in the reads' mode; false when it could not. */
bool readSurface(
    const internal::Instance& instance,
    bool synced,
    SurfaceId surfaceId,
    const std::function<void(const LoomRenderer&, const LoomBinding::SurfaceConstraints&)>& read) {
  bool ran = false;
  instance.withBinding([&](LoomBinding& binding) { ran = binding.apiRead(surfaceId, synced, read); });
  return ran;
}

} // namespace

std::optional<SurfaceId> Reads::surfaceOf(Tag tag) const {
  std::vector<SurfaceId> surfaces;
  instance_->withBinding([&](LoomBinding& binding) {
    binding.apiForEachSurface([&](SurfaceId surfaceId) { surfaces.push_back(surfaceId); });
  });
  for (const auto surfaceId : surfaces) {
    bool found = false;
    readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
      found = renderer.exists(tag);
    });
    if (found) {
      return surfaceId;
    }
  }
  return std::nullopt;
}

std::optional<Size> Reads::surfaceSize(SurfaceId surfaceId) const {
  std::optional<Size> size;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints& constraints) {
    // The root's laid-out size, as Fabric reports a surface's layout size: a
    // surface sized to its content has no bounded maximum to report.
    LoomRenderer::Metrics root;
    if (const auto rootTag = renderer.rootTag(); rootTag != 0 &&
        renderer.metricsRelativeTo(rootTag, 0, root, /*includeTransform*/ false, /*includeScroll*/ false) &&
        std::isfinite(root.width) && std::isfinite(root.height)) {
      size = Size{root.width, root.height};
      return;
    }
    size = Size{constraints.maxWidth, constraints.maxHeight};
  });
  return size;
}

std::optional<bool> Reads::hasNode(SurfaceId surfaceId, Tag tag) const {
  std::optional<bool> found;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    found = renderer.exists(tag);
  });
  return found;
}

std::optional<folly::dynamic> Reads::props(SurfaceId surfaceId, Tag tag) const {
  std::optional<folly::dynamic> result;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    if (auto info = renderer.fabricNodeInfo(tag); info.has_value() && info->props != nullptr) {
      result = *info->props;
    }
  });
  return result;
}

std::optional<LayoutMetrics> Reads::layoutRelativeTo(SurfaceId surfaceId, Tag tag, std::optional<Tag> ancestor) const {
  std::optional<LayoutMetrics> result;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    LoomRenderer::Metrics metrics;
    if (renderer.metricsRelativeTo(tag, ancestor.value_or(0), metrics)) {
      result = toLayoutMetrics(metrics);
    }
  });
  return result;
}

std::optional<LayoutMetrics> Reads::layout(SurfaceId surfaceId, Tag tag) const {
  std::optional<LayoutMetrics> result;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    LoomRenderer::Metrics metrics;
    const auto parentTag = renderer.parentOf(tag);
    if (parentTag != 0 &&
        renderer.metricsRelativeTo(tag, parentTag, metrics, /*includeTransform*/ false, /*includeScroll*/ false)) {
      result = toLayoutMetrics(metrics);
    }
  });
  return result;
}

std::optional<std::string> Reads::componentName(SurfaceId surfaceId, Tag tag) const {
  std::optional<std::string> result;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    if (renderer.exists(tag)) {
      result = renderer.componentName(tag);
    }
  });
  return result;
}

std::optional<std::vector<Tag>> Reads::children(SurfaceId surfaceId, Tag tag) const {
  std::optional<std::vector<Tag>> result;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    if (renderer.exists(tag)) {
      result = renderer.childrenOf(tag);
    }
  });
  return result;
}

std::optional<Tag> Reads::parent(SurfaceId surfaceId, Tag tag) const {
  std::optional<Tag> result;
  readSurface(*instance_, synced_, surfaceId, [&](const LoomRenderer& renderer, const LoomBinding::SurfaceConstraints&) {
    if (renderer.exists(tag)) {
      if (auto parentTag = renderer.parentOf(tag); parentTag != 0) {
        result = parentTag;
      }
    }
  });
  return result;
}

// ---- Runtime ----------------------------------------------------------------------------------------

void Runtime::forEachSurface(const std::function<void(SurfaceId)>& visit) const {
  std::vector<SurfaceId> surfaces;
  instance_->withBinding([&](LoomBinding& binding) {
    binding.apiForEachSurface([&](SurfaceId surfaceId) { surfaces.push_back(surfaceId); });
  });
  // Outside the binding's gate: `visit` may call back into this handle.
  for (const auto surfaceId : surfaces) {
    visit(surfaceId);
  }
}

std::optional<SurfaceId> Runtime::surfaceOf(Tag tag) const {
  std::optional<SurfaceId> result;
  instance_->withBinding([&](LoomBinding& binding) { result = binding.apiSurfaceOf(tag); });
  return result;
}

void Runtime::updateProps(SurfaceId surfaceId, Tag tag, folly::dynamic props) const {
  instance_->withBinding([&](LoomBinding& binding) { binding.apiUpdateProps(surfaceId, tag, std::move(props)); });
}

void Runtime::updateAnimatedProps(SurfaceId surfaceId, Tag tag, AnimatedPropBag props) const {
  // Nothing but Loom writes a library's bag to the view (unlike the platform animation backend, which
  // applies paint itself): the lane writes paint to the view and lays out layout props.
  auto packed = animationbackend::packAnimatedProps(props);
  instance_->withBinding([&](LoomBinding& binding) {
    binding.apiRecordNativeProps(surfaceId, tag, std::move(packed), /*knownPaint=*/false, /*viewApplied=*/false);
  });
}

void Runtime::updateNativeProps(SurfaceId surfaceId, Tag tag, folly::dynamic props) const {
  instance_->withBinding([&](LoomBinding& binding) {
    binding.apiRecordNativeProps(surfaceId, tag, std::move(props), /*knownPaint=*/false, /*viewApplied=*/false);
  });
}

Subscription Runtime::onFrameApplied(FrameListener listener) const {
  auto& instance = *instance_;
  uint64_t id;
  {
    std::lock_guard lock(instance.mutex);
    id = instance.next++;
    instance.frame.emplace(id, std::move(listener));
    instance.frameCount.fetch_add(1, std::memory_order_relaxed);
  }
  return Subscription([weak = std::weak_ptr<internal::Instance>(instance_), id] {
    if (auto instance = weak.lock()) {
      std::lock_guard lock(instance->mutex);
      if (instance->frame.erase(id) != 0) {
        instance->frameCount.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  });
}

Subscription Runtime::onLayoutCommitted(CommitListener listener) const {
  auto& instance = *instance_;
  uint64_t id;
  {
    std::lock_guard lock(instance.mutex);
    id = instance.next++;
    instance.commit.emplace(id, std::move(listener));
    instance.commitCount.fetch_add(1, std::memory_order_relaxed);
  }
  return Subscription([weak = std::weak_ptr<internal::Instance>(instance_), id] {
    if (auto instance = weak.lock()) {
      std::lock_guard lock(instance->mutex);
      if (instance->commit.erase(id) != 0) {
        instance->commitCount.fetch_sub(1, std::memory_order_relaxed);
      }
    }
  });
}

void Runtime::beforeNextFrame(SurfaceId surfaceId, std::function<void()> callback) const {
  auto& instance = *instance_;
  std::lock_guard lock(instance.mutex);
  instance.beforeFrame[surfaceId].push_back(std::move(callback));
  instance.beforeCount.fetch_add(1, std::memory_order_relaxed);
}

bool Runtime::updateState(SurfaceId surfaceId, Tag tag, const StateUpdate& update) const {
  if (update.family == nullptr || update.family->getSurfaceId() != surfaceId || update.family->getTag() != tag) {
    return false;
  }
  bool delivered = false;
  instance_->withBinding([&](LoomBinding& binding) { delivered = binding.apiUpdateState(surfaceId, update); });
  return delivered;
}

std::optional<int> Runtime::setting(LoomSettingId id) const {
  if (id >= LoomSettingId::Count) {
    return std::nullopt;
  }
  return LoomSettings::get(id);
}

} // namespace facebook::react::loom

#endif // LOOM_RENDERER
