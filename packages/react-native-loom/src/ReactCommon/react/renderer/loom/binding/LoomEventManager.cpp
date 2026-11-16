/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "LoomEventManager.h"

#include <react/renderer/core/DynamicEventPayload.h>
#include <react/renderer/loom/host/LoomPlatformTraits.h>
#include "../worker/LoomWorkerProfile.h"

#include <mutex>
#include <unordered_set>
#include <utility>
namespace facebook::react {

LoomEventManager::LoomEventManager(
    EventDispatcher::Shared eventDispatcher)
    : eventDispatcher_(std::move(eventDispatcher)) {}

LoomEventManager::~LoomEventManager() {
  std::unordered_set<uint64_t> ownedTargets;
  {
    std::lock_guard lock(targetsMutex_);
    ownedTargets.reserve(targets_.size());
    for (const auto& entry : targets_) {
      ownedTargets.insert(entry.first);
    }
    targets_.clear();
  }
  std::lock_guard lock(pointerCaptureMutex_);
  for (auto iterator = pointerCaptureTargets_.begin();
       iterator != pointerCaptureTargets_.end();) {
    if (ownedTargets.count(iterator->second) != 0) {
      iterator = pointerCaptureTargets_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

std::unordered_map<uint64_t, uint64_t>
    LoomEventManager::pointerCaptureTargets_;
std::mutex LoomEventManager::pointerCaptureMutex_;

uint64_t LoomEventManager::key(SurfaceId surfaceId, Tag tag) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(surfaceId)) << 32) |
      static_cast<uint32_t>(tag);
}

void LoomEventManager::registerTarget(
    SurfaceId surfaceId,
    Tag tag,
    InstanceHandle::Shared instanceHandle) {
  if (instanceHandle == nullptr) {
    return;
  }
  auto target = std::make_shared<EventTarget>(instanceHandle, surfaceId);
  target->setEnabled(true);
  std::lock_guard lock(targetsMutex_);
  targets_[key(surfaceId, tag)] = std::move(target);
}

void LoomEventManager::unregisterTarget(SurfaceId surfaceId, Tag tag) {
  const auto targetKey = key(surfaceId, tag);
  {
    std::lock_guard lock(targetsMutex_);
    targets_.erase(targetKey);
  }
  std::lock_guard captureLock(pointerCaptureMutex_);
  for (auto iterator = pointerCaptureTargets_.begin();
       iterator != pointerCaptureTargets_.end();) {
    if (iterator->second == targetKey) {
      iterator = pointerCaptureTargets_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

void LoomEventManager::clearSurface(SurfaceId surfaceId) {
  std::lock_guard lock(pointerCaptureMutex_);
  const auto surfacePrefix =
      static_cast<uint64_t>(static_cast<uint32_t>(surfaceId)) << 32;
  for (auto iterator = pointerCaptureTargets_.begin();
       iterator != pointerCaptureTargets_.end();) {
    const auto captureKey = iterator->first;
    const auto targetKey = iterator->second;
    if ((captureKey & 0xffffffff00000000ULL) == surfacePrefix ||
        (targetKey & 0xffffffff00000000ULL) == surfacePrefix) {
      iterator = pointerCaptureTargets_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

void LoomEventManager::setPointerCapture(
    SurfaceId surfaceId,
    Tag tag,
    int32_t pointerId) {
  std::lock_guard lock(pointerCaptureMutex_);
  pointerCaptureTargets_[key(surfaceId, pointerId)] = key(surfaceId, tag);
}

void LoomEventManager::releasePointerCapture(
    SurfaceId surfaceId,
    Tag tag,
    int32_t pointerId) {
  const auto captureKey = key(surfaceId, pointerId);
  std::lock_guard lock(pointerCaptureMutex_);
  const auto iterator = pointerCaptureTargets_.find(captureKey);
  if (iterator != pointerCaptureTargets_.end() &&
      iterator->second == key(surfaceId, tag)) {
    pointerCaptureTargets_.erase(iterator);
  }
}

bool LoomEventManager::hasPointerCapture(
    SurfaceId surfaceId,
    Tag tag,
    int32_t pointerId) {
  std::lock_guard lock(pointerCaptureMutex_);
  const auto capture = pointerCaptureTargets_.find(key(surfaceId, pointerId));
  return capture != pointerCaptureTargets_.end() &&
      capture->second == key(surfaceId, tag);
}

void LoomEventManager::dispatch(
    SurfaceId surfaceId,
    Tag tag,
    const std::string& type,
    folly::dynamic payload,
    RawEvent::Category category,
    bool unique,
    HighResTimeStamp eventTimestamp,
    bool synchronous) {
  if (payload.isObject()) {
    const auto* pointerId = payload.get_ptr("pointerId");
    if (pointerId != nullptr && pointerId->isNumber()) {
      std::lock_guard lock(pointerCaptureMutex_);
      const auto capture = pointerCaptureTargets_.find(
          key(surfaceId, static_cast<int32_t>(pointerId->asInt())));
      if (capture != pointerCaptureTargets_.end()) {
        tag = static_cast<Tag>(static_cast<uint32_t>(capture->second));
      }
    }
  }
  SharedEventTarget target;
  {
    std::lock_guard lock(targetsMutex_);
    auto iterator = targets_.find(key(surfaceId, tag));
    if (iterator == targets_.end()) {
#if LOOM_PROFILE
      if (LoomWorkerProfile::enabled() &&
          (type == "topTouchStart" || type == "topTouchEnd" ||
           type == "topTouchCancel")) {
        loomLog(
            LoomLogLevel::Profile,
            "LoomDirectEvent",
            "missing-target surface=%d tag=%d type=%s targets=%zu",
            static_cast<int>(surfaceId),
            static_cast<int>(tag),
            type.c_str(),
            targets_.size());
      }
#endif
      return;
    }
    target = iterator->second;
  }
  if (eventDispatcher_ == nullptr) {
    return;
  }
#if LOOM_PROFILE
  if (LoomWorkerProfile::enabled() &&
      (type == "topTouchStart" || type == "topTouchEnd" ||
       type == "topTouchCancel")) {
    loomLog(
        LoomLogLevel::Profile,
        "LoomDirectEvent",
        "enqueue surface=%d tag=%d type=%s sync=%d",
        static_cast<int>(surfaceId),
        static_cast<int>(tag),
        type.c_str(),
        synchronous);
  }
#endif
  RawEvent event(
      type,
      DynamicEventPayload::create(std::move(payload)),
      target,
      std::weak_ptr<const ShadowNodeFamily>{},
      category,
      unique,
      eventTimestamp);
  if (unique) {
    eventDispatcher_->dispatchUniqueEvent(std::move(event));
  } else {
    eventDispatcher_->dispatchEvent(std::move(event));
  }
  if (!synchronous) {
    return;
  }
  if (kLoomJavaScriptOnUIThread) {
    eventDispatcher_->experimental_flushSync();
    return;
  }
  // A synchronous event blocks the calling (UI) thread until JavaScript has
  // handled it. With JavaScript on its own thread that wait can deadlock against
  // a JS thread that is itself waiting for the UI thread to apply worker
  // results, so the event is delivered asynchronously instead. Waits go from JS
  // to UI only.
  static std::once_flag logged;
  std::call_once(logged, [] {
    loomLog(
        LoomLogLevel::Info,
        "LoomDirectEvent",
        "synchronous events are delivered asynchronously when JavaScript is not on the UI thread");
  });
}

} // namespace facebook::react
