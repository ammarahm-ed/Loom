/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include <atomic>
#include <optional>
#include <unordered_map>

#include <folly/dynamic.h>
#include <react/renderer/core/Props.h>
#include <react/renderer/mounting/ShadowTree.h>

namespace facebook::react {

/**
 * The raw props each props clone applied while it was active, keyed by the
 * props object the clone produced (`ConcreteComponentDescriptor::cloneProps`).
 * Loom runs a library's shadow-tree commit with one active to learn which props
 * the commit changed, as the props Loom's renderer understands.
 */
struct LoomRawPropsCapture {
  std::unordered_map<const Props*, folly::dynamic> rawProps;
};

/** Makes `capture` the active capture of this thread for its lifetime. */
class LoomRawPropsCaptureScope {
 public:
  explicit LoomRawPropsCaptureScope(LoomRawPropsCapture& capture);
  ~LoomRawPropsCaptureScope();
  LoomRawPropsCaptureScope(const LoomRawPropsCaptureScope&) = delete;
  LoomRawPropsCaptureScope& operator=(const LoomRawPropsCaptureScope&) = delete;

 private:
  LoomRawPropsCapture* previous_;
};

/**
 * The hook `ShadowTree::commit` consults on Loom builds (ShadowTree.cpp);
 * `installLoomFabricBridge` points it at `LoomBinding::commitOnDemand`.
 */
using LoomShadowTreeCommitInterceptor = std::optional<ShadowTreeCommitStatus> (*)(
    const ShadowTree& shadowTree,
    const ShadowTreeCommitTransaction& transaction);
std::atomic<LoomShadowTreeCommitInterceptor>& loomShadowTreeCommitInterceptor();

/**
 * The hook `ShadowTree::notifyDelegatesOfUpdates` consults on Loom builds;
 * true when Loom handled the update (a layout-animation frame).
 */
using LoomShadowTreeNotifyInterceptor = bool (*)(SurfaceId surfaceId);
std::atomic<LoomShadowTreeNotifyInterceptor>& loomShadowTreeNotifyInterceptor();

/** Installs the Fabric bridge's hooks into the layers below the UI manager. */
void installLoomFabricBridge();

/** `LoomBinding::recordAnimationProps`, for the animation backend. */
void loomRecordAnimationProps(Tag tag, folly::dynamic props, bool viewApplied);

/** `LoomBinding::flushAnimationFrames`, for the animation backend. */
void loomFlushAnimationFrames();

} // namespace facebook::react
