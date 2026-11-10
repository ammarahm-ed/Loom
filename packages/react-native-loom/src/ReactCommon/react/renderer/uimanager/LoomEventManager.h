/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include <folly/dynamic.h>
#include <react/renderer/core/EventDispatcher.h>
#include <react/renderer/core/EventEmitter.h>
#include <react/renderer/core/RawEvent.h>
#include <react/renderer/core/ReactPrimitives.h>

namespace facebook::react {

/** Direct-renderer event target and pointer-capture registry. */
class LoomEventManager final {
 public:
  explicit LoomEventManager(EventDispatcher::Shared eventDispatcher);
  ~LoomEventManager();
  void registerTarget(SurfaceId, Tag, InstanceHandle::Shared);
  void unregisterTarget(SurfaceId, Tag);
  static void clearSurface(SurfaceId);
  static void setPointerCapture(SurfaceId, Tag, int32_t pointerId);
  static void releasePointerCapture(SurfaceId, Tag, int32_t pointerId);
  static bool hasPointerCapture(SurfaceId, Tag, int32_t pointerId);
  void dispatch(
      SurfaceId,
      Tag,
      const std::string&,
      folly::dynamic,
      RawEvent::Category = RawEvent::Category::Unspecified,
      bool unique = false,
      HighResTimeStamp eventTimestamp = HighResTimeStamp::now(),
      bool synchronous = false);

 private:
  static uint64_t key(SurfaceId, Tag);
  EventDispatcher::Shared eventDispatcher_;
  std::mutex targetsMutex_;
  std::unordered_map<uint64_t, SharedEventTarget> targets_;
  static std::unordered_map<uint64_t, uint64_t> pointerCaptureTargets_;
  static std::mutex pointerCaptureMutex_;
};

} // namespace facebook::react
