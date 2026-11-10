/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the LICENSE file in the root directory.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <react/renderer/uimanager/UIManagerDelegate.h>

namespace facebook::react {

/**
 * A `UIManagerDelegate` that does nothing, for tests that need a `UIManager`
 * with a delegate attached but do not care what it does.
 *
 * The interface has 16 pure virtuals, so a test cannot simply declare one; and
 * `LoomBinding` refuses to construct in worker mode without a
 * delegate that returns a UI executor, which is what most of the Loom renderer
 * tests need. Override what a test cares about and leave the rest.
 */
struct TestUIManagerDelegate : public UIManagerDelegate {
  /** Runs work inline, which is what tests want instead of a real UI thread. */
  std::function<void(std::function<void()>)> uiExecutor = [](std::function<void()> work) {
    work();
  };
  bool canMeasureTextOnWorker{false};
  std::function<void(SurfaceId, const std::string&)> failSurface;

  // Direct-renderer hooks the binding actually consults.
  std::function<void(std::function<void()>)> loomGetUIExecutor()
      override {
    return uiExecutor;
  }

  bool loomCanMeasureTextOnWorker() override {
    return canMeasureTextOnWorker;
  }

  void loomFailSurface(
      SurfaceId surfaceId,
      const std::string& reason) override {
    if (failSurface) {
      failSurface(surfaceId, reason);
    }
  }

  // The rest of the interface, unused by these tests.
  void uiManagerDidFinishTransaction(
      std::shared_ptr<const MountingCoordinator>,
      bool) override {}
  void uiManagerDidCreateShadowNode(const ShadowNode&) override {}
  void uiManagerDidDispatchCommand(
      const std::shared_ptr<const ShadowNode>&,
      const std::string&,
      const folly::dynamic&) override {}
  void uiManagerDidSendAccessibilityEvent(
      const std::shared_ptr<const ShadowNode>&,
      const std::string&) override {}
  void uiManagerDidSetIsJSResponder(
      const std::shared_ptr<const ShadowNode>&,
      bool,
      bool) override {}
  void uiManagerShouldSynchronouslyUpdateViewOnUIThread(
      Tag,
      const folly::dynamic&) override {}
  void uiManagerDidUpdateShadowTree(
      const std::unordered_map<Tag, folly::dynamic>&) override {}
  void uiManagerShouldAddEventListener(
      std::shared_ptr<const EventListener>) override {}
  void uiManagerShouldRemoveEventListener(
      const std::shared_ptr<const EventListener>&) override {}
  void uiManagerDidStartSurface(const ShadowTree&) override {}
  void uiManagerDidFinishReactCommit(const ShadowTree&) override {}
  void uiManagerDidPromoteReactRevision(const ShadowTree&) override {}
  void uiManagerShouldAddOnSurfaceStartCallback(
      OnSurfaceStartCallback&&) override {}
  void uiManagerDidCaptureViewSnapshot(Tag, SurfaceId) override {}
  void uiManagerDidSetViewSnapshot(Tag, Tag, SurfaceId) override {}
  void uiManagerDidClearPendingSnapshots() override {}
};

} // namespace facebook::react