/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#if __has_include(<react/renderer/LoomConfig.h>)
#include <react/renderer/LoomConfig.h>
#endif

#if defined(LOOM_RENDERER) && LOOM_RENDERER

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <react/loom/Loom.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/utils/ContextContainer.h>

#include <react/renderer/loom/binding/LoomBinding.h>

namespace facebook::react::loom::internal {

/**
 * The library API's state for one React instance: what a `loom::Runtime` handle points at. Stored in
 * the instance's ContextContainer, so a TurboModule can get it before JavaScript (and the binding)
 * exists; the binding attaches itself when it is created and detaches when it is destroyed.
 *
 * Listener state is per instance and per surface: nothing here is shared between React instances, and
 * a released surface leaves nothing behind (`releaseSurface`).
 */
struct Instance {
  static constexpr const char* kContextKey = "LoomApiInstance";

  /** The instance's API state, created on first use. */
  static std::shared_ptr<Instance> in(const ContextContainer& container);

  /**
   * Runs `body` with the attached binding, which cannot be destroyed while it runs. False when no
   * binding is attached (before JavaScript starts, or after teardown).
   */
  bool withBinding(const std::function<void(LoomBinding&)>& body) const;

  /** The binding of this instance attaches when it is created and detaches when it is destroyed. */
  void attach(std::weak_ptr<LoomBinding::Lifetime> lifetime);
  void detach(const LoomBinding::Lifetime* lifetime);

  // ---- Listeners -------------------------------------------------------------------------------------
  std::mutex mutex;
  uint64_t next{1};
  std::unordered_map<uint64_t, Runtime::CommitListener> commit;
  std::unordered_map<uint64_t, Runtime::FrameListener> frame;
  std::unordered_map<SurfaceId, std::unordered_set<Tag>> changedSinceFrame;
  std::unordered_map<SurfaceId, std::vector<std::function<void()>>> beforeFrame;
  std::atomic<int> commitCount{0};
  std::atomic<int> frameCount{0};
  std::atomic<int> beforeCount{0};

 private:
  mutable std::mutex bindingMutex_;
  // Weak: the binding's lifetime gate keeps it alive (and its pointer valid) for the duration of
  // `withBinding`'s body.
  std::weak_ptr<LoomBinding::Lifetime> lifetime_;
};

// What the renderer tells the public API. Each is one relaxed atomic load when nobody subscribed.

/** A commit of `surfaceId` completed layout (commit thread); `changed` is built only when asked for. */
bool wantsLayoutCommitted(const Instance& instance);
void notifyLayoutCommitted(Instance& instance, SurfaceId surfaceId, std::vector<Tag> changed);

/** Just before a batch of `surfaceId` is applied (UI thread): runs that surface's `beforeNextFrame` callbacks. */
void runBeforeFrame(Instance& instance, SurfaceId surfaceId);

/** A frame's batch of `surfaceId` was applied (UI thread). */
void notifyFrameApplied(Instance& instance, SurfaceId surfaceId);

/** The surface was released: drop its pending callbacks and change sets. */
void releaseSurface(Instance& instance, SurfaceId surfaceId);

} // namespace facebook::react::loom::internal

#endif // LOOM_RENDERER
