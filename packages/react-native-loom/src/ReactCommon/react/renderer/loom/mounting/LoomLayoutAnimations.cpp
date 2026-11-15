/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomLayoutAnimations.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace facebook::react {

namespace {

struct Drivers {
  std::mutex mutex;
  std::unordered_map<SurfaceId, std::weak_ptr<LoomLayoutAnimationDriver>> bySurface;
  /** Lets surfaces without a driver skip the lock. */
  std::atomic<size_t> count{0};
};

Drivers& drivers() {
  static Drivers instance;
  return instance;
}

} // namespace

void loomRegisterLayoutAnimationDriver(
    SurfaceId surfaceId,
    std::weak_ptr<LoomLayoutAnimationDriver> driver) {
  auto& registry = drivers();
  std::lock_guard lock(registry.mutex);
  registry.bySurface[surfaceId] = std::move(driver);
  registry.count.store(registry.bySurface.size(), std::memory_order_release);
}

void loomUnregisterLayoutAnimationDriver(
    SurfaceId surfaceId,
    const LoomLayoutAnimationDriver* driver) {
  auto& registry = drivers();
  std::lock_guard lock(registry.mutex);
  auto it = registry.bySurface.find(surfaceId);
  if (it == registry.bySurface.end()) {
    return;
  }
  auto current = it->second.lock();
  if (current == nullptr || current.get() == driver) {
    registry.bySurface.erase(it);
  }
  registry.count.store(registry.bySurface.size(), std::memory_order_release);
}

std::shared_ptr<LoomLayoutAnimationDriver> loomLayoutAnimationDriver(
    SurfaceId surfaceId) {
  auto& registry = drivers();
  if (registry.count.load(std::memory_order_acquire) == 0) {
    return nullptr;
  }
  std::lock_guard lock(registry.mutex);
  auto it = registry.bySurface.find(surfaceId);
  return it == registry.bySurface.end() ? nullptr : it->second.lock();
}

} // namespace facebook::react
