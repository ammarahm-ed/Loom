/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The two JavaScript-thread modes share one build; the suite runs once per mode
// (LOOM_TEST_JS_THREAD). Each mode must leave the other's machinery untouched:
//   - main mode never takes a background-only wait or posts the text-capability seeding to the UI thread;
//   - background mode never registers or runs the frame drain or after-frame hook, and
//     its UI thread never drains the worker.

#include <gtest/gtest.h>

#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <react/renderer/components/root/RootComponentDescriptor.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/core/EventDispatcher.h>
#include <react/renderer/loom/binding/LoomBinding.h>
#include <react/renderer/loom/tests/LoomTestMode.h>
#include <react/renderer/runtimescheduler/RuntimeScheduler_Modern.h>
#include <react/renderer/uimanager/UIManager.h>

#include "../LoomHeadlessHost.h"

namespace facebook::react {

namespace {

class LoomModeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    LoomModeCounters::reset();
    FrameCallbackScope::afterFrameCallbacks() = {};
  }
  static bool mainMode() {
    return kLoomTestJavaScriptOnUIThread;
  }

  /** A binding with a registered host and no delegate, as the headless binding suite builds it. */
  std::shared_ptr<LoomBinding> makeBinding() {
    contextContainer_ = std::make_shared<ContextContainer>();
    host_ = LoomHeadlessHost::registerIn(*contextContainer_);
    ComponentDescriptorProviderRegistry providerRegistry{};
    auto registry = providerRegistry.createComponentDescriptorRegistry(
        ComponentDescriptorParameters{
            .eventDispatcher = EventDispatcher::Shared{},
            .contextContainer = contextContainer_,
            .flavor = nullptr});
    providerRegistry.add(concreteComponentDescriptorProvider<RootComponentDescriptor>());
    providerRegistry.add(concreteComponentDescriptorProvider<ViewComponentDescriptor>());
    RuntimeExecutor runtimeExecutor = [](std::function<void(jsi::Runtime&)>&&) {};
    uiManager_ = std::make_shared<UIManager>(runtimeExecutor, contextContainer_);
    uiManager_->setComponentDescriptorRegistry(std::move(registry));
    return std::make_shared<LoomBinding>(uiManager_);
  }

  std::shared_ptr<ContextContainer> contextContainer_;
  std::shared_ptr<LoomHeadlessHost> host_;
  std::shared_ptr<UIManager> uiManager_;
};

} // namespace

TEST_F(LoomModeTest, TheAccessorReportsTheModeTheRunWasStartedIn) {
  EXPECT_EQ(loomJavaScriptOnUIThread(), mainMode());
  EXPECT_EQ(LoomWorker::javaScriptOnUIThread(), mainMode());
}

TEST_F(LoomModeTest, TheHostSetterIsTheProgrammaticEntryPoint) {
  // Setting what is already fixed is harmless (a host may repeat its config).
  LoomHost::setJavaScriptOnUIThread(mainMode());
  EXPECT_EQ(loomJavaScriptOnUIThread(), mainMode());
}

TEST_F(LoomModeTest, TheFrameDrainHookIsRegisteredOnlyInMainMode) {
  auto binding = makeBinding();
  if (mainMode()) {
    EXPECT_TRUE(static_cast<bool>(FrameCallbackScope::afterFrameCallbacks()));
    EXPECT_EQ(LoomModeCounters::afterFrameHookInstalls.load(), 1u);
  } else {
    EXPECT_FALSE(static_cast<bool>(FrameCallbackScope::afterFrameCallbacks()));
    EXPECT_EQ(LoomModeCounters::afterFrameHookInstalls.load(), 0u);
  }
  binding.reset();
  EXPECT_FALSE(static_cast<bool>(FrameCallbackScope::afterFrameCallbacks()));
}

TEST_F(LoomModeTest, AFrameScopeRunsTheDrainOnlyInMainMode) {
  auto binding = makeBinding();
  { FrameCallbackScope scope; }
  if (mainMode()) {
    EXPECT_EQ(LoomModeCounters::afterFrameHookRuns.load(), 1u);
    EXPECT_EQ(LoomModeCounters::frameDrains.load(), 1u);
  } else {
    EXPECT_EQ(LoomModeCounters::afterFrameHookRuns.load(), 0u);
    EXPECT_EQ(LoomModeCounters::frameDrains.load(), 0u);
  }
}

TEST_F(LoomModeTest, CommittingTouchesOnlyTheCurrentModesMachinery) {
  auto binding = makeBinding();
  LoomBinding::setSurfaceConstraints(1, 200, 200, 200, 200, false, 0, 0);
  // Commit through the worker and apply on the (test) UI thread.
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  host_->drainUIQueue();
  if (mainMode()) {
    EXPECT_EQ(LoomModeCounters::jsToUIWaits.load(), 0u);
    EXPECT_EQ(LoomModeCounters::uiCapabilityPosts.load(), 0u);
  } else {
    EXPECT_EQ(LoomModeCounters::frameDrains.load(), 0u);
    EXPECT_EQ(LoomModeCounters::afterFrameHookRuns.load(), 0u);
  }
}

TEST_F(LoomModeTest, TheRuntimeSchedulerFixesTheModeWhenItIsCreated) {
  RuntimeExecutor executor = [](std::function<void(jsi::Runtime&)>&&) {};
  RuntimeScheduler_Modern scheduler(
      executor, [] { return HighResTimeStamp::now(); }, [](jsi::Runtime&, jsi::JSError&) {});
  EXPECT_EQ(scheduler.jsOnUIThreadForTesting(), mainMode());
  // Outside a task neither mode slices: the main-thread-only paths stay untouched.
  EXPECT_FALSE(scheduler.getShouldYield());
  EXPECT_EQ(LoomModeCounters::schedulerMainPaths.load(), 0u);
}

} // namespace facebook::react
