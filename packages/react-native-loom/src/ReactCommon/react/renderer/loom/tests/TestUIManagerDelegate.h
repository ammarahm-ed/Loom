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

#include <react/renderer/loom/host/LoomHost.h>
#include <react/renderer/uimanager/UIManagerDelegate.h>

namespace facebook::react {

/**
 * A `LoomHost` for tests that need a `LoomBinding` but not a platform: work
 * posted to the UI executor runs inline (tests want that instead of a real UI
 * thread), batches are dropped, and there are no text services. Override what
 * a test cares about.
 */
struct TestLoomHost : public LoomHost {
  std::function<void(std::function<void()>)> uiExecutor_ = [](std::function<void()> work) {
    work();
  };
  std::function<void(SurfaceId, const std::string&)> failSurface_;

  /** The traits the test binary installed; registering the host keeps them. */
  LoomPlatformTraits traits() const override {
    return LoomPlatformTraits::current();
  }
  std::function<void(std::function<void()>)> uiExecutor() override {
    return uiExecutor_;
  }
  void applyBatch(SurfaceId, LoomMountBatch) override {}
  void failSurface(SurfaceId surfaceId, const std::string& reason) override {
    if (failSurface_) {
      failSurface_(surfaceId, reason);
    }
  }
};

/**
 * A `UIManagerDelegate` that does nothing, for tests that need a `UIManager`
 * with a delegate attached but do not care what it does.
 *
 * The interface has 12 pure virtuals, so a test cannot simply declare one.
 */
struct TestUIManagerDelegate : public UIManagerDelegate {
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