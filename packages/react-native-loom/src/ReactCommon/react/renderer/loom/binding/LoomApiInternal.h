/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <react/renderer/core/ReactPrimitives.h>

namespace facebook::react::loom::internal {

// What the renderer tells the public API (react/loom/Loom.h, LoomApi.cpp). Each is one relaxed atomic
// load when nobody subscribed.

/** A commit of `surfaceId` completed layout (commit thread); `changed` is built only when asked for. */
bool wantsLayoutCommitted();
void notifyLayoutCommitted(SurfaceId surfaceId, std::vector<Tag> changed);

/** A frame's batch of `surfaceId` was applied (UI thread). */
void notifyFrameApplied(SurfaceId surfaceId);

/** Before the frame's animation props and batch are applied (UI thread): runs `beforeNextFrame` callbacks. */
void runBeforeFrame();

} // namespace facebook::react::loom::internal
