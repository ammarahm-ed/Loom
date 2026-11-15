/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <folly/dynamic.h>

#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/graphics/Rect.h>
#include <react/renderer/loom/mounting/LoomMountBatch.h>

namespace facebook::react {

/**
 * What a driver can read about its surface. Shared element transitions need
 * window frames of views outside the batch.
 */
class LoomLayoutAnimationGeometry {
 public:
  virtual ~LoomLayoutAnimationGeometry() = default;

  /**
   * `tag`'s layout frame in the surface from the renderer, without transforms
   * or scroll offsets. Only in `processBatch`, where the renderer matches the
   * batch; elsewhere it is empty.
   */
  virtual std::optional<Rect> layoutFrameInSurface(Tag tag) const = 0;

  /** `tag`'s parent in the renderer (0 if unknown). Only in `processBatch`. */
  virtual Tag parentOf(Tag tag) const = 0;

  /** How many views are mounted in `tag`. Only in `processBatch`. */
  virtual size_t mountedChildCount(Tag tag) const = 0;

  /**
   * `tag`'s component name and props in the renderer, also for a node that
   * mounts no view of its own (flattened, `display: contents`). Borrowed until
   * `processBatch` returns; null if unknown. Only in `processBatch`.
   */
  virtual const std::string* componentNameOf(Tag tag) const = 0;
  virtual const folly::dynamic* propsOf(Tag tag) const = 0;

  /**
   * Origin of `tag`'s mounted native view in the surface (scroll offsets,
   * native headers and transforms included). Read on the UI thread: from
   * another thread this waits for it, so never call it holding a lock the UI
   * thread can wait for.
   */
  virtual std::optional<Point> nativeOriginInSurface(Tag tag) const = 0;
};

/**
 * A layout-animation driver for one surface (the Reanimated adapter provides
 * one). Loom's renderer has no shadow tree and no mutation stream for a
 * MountingOverrideDelegate to rewrite; instead it hands the driver each mount
 * batch before the platform applies it, and lets it append the frames of the
 * animations it runs. `processBatch` runs where the renderer commits (the UI
 * thread when JavaScript runs there, its worker otherwise); `frame` runs on
 * the UI thread.
 *
 * A surface without a driver pays nothing: the lookup below is one relaxed
 * atomic load while no driver is registered.
 */
class LoomLayoutAnimationDriver {
 public:
  virtual ~LoomLayoutAnimationDriver() = default;

  /**
   * Called with a batch about to be mounted. The driver may hold operations
   * back (an exiting view stays mounted), and append operations (the first
   * frame of an entering view). It must leave every other operation in order.
   */
  virtual void processBatch(
      SurfaceId surfaceId,
      LoomMountBatch& batch,
      const LoomLayoutAnimationGeometry& geometry) = 0;

  /**
   * Called when the animations of the surface have a new frame. The driver
   * appends the updates (props, frames) and the held removals that are now due.
   */
  virtual void frame(
      SurfaceId surfaceId,
      LoomMountBatch& batch,
      const LoomLayoutAnimationGeometry& geometry) = 0;
};

/** Registers the driver of `surfaceId`; it is held weakly. */
void loomRegisterLayoutAnimationDriver(
    SurfaceId surfaceId,
    std::weak_ptr<LoomLayoutAnimationDriver> driver);

/** Drops the driver of `surfaceId` if it is still `driver`. */
void loomUnregisterLayoutAnimationDriver(
    SurfaceId surfaceId,
    const LoomLayoutAnimationDriver* driver);

/** The driver of `surfaceId`, or null. */
std::shared_ptr<LoomLayoutAnimationDriver> loomLayoutAnimationDriver(
    SurfaceId surfaceId);

} // namespace facebook::react
