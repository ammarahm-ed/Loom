/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <react/renderer/componentregistry/ComponentDescriptorProviderRegistry.h>
#include <react/renderer/components/root/RootComponentDescriptor.h>
#include <react/renderer/components/view/ViewComponentDescriptor.h>
#include <react/renderer/core/EventDispatcher.h>
#include <react/renderer/uimanager/LoomBinding.h>
#include <react/renderer/uimanager/UIManager.h>

#include "../LoomHeadlessHost.h"

namespace facebook::react {

// Reaches the binding's publication path, which the host API normally drives.
struct LoomBindingTestAccess {
  using Draft = LoomBinding::DraftNode;

  static std::shared_ptr<Draft> draft(
      SurfaceId surfaceId,
      uint64_t generation,
      uint64_t identity,
      Tag tag,
      std::string componentName,
      folly::dynamic props) {
    auto node = std::make_shared<Draft>();
    node->surfaceId = surfaceId;
    node->surfaceGeneration = generation;
    node->identity = identity;
    node->tag = tag;
    node->componentName =
        std::make_shared<const std::string>(std::move(componentName));
    node->initialProps =
        std::make_shared<const folly::dynamic>(std::move(props));
    node->hasInitialProps = true;
    node->roles = LoomPlatformTraits::current().components.classify(
        *node->componentName);
    return node;
  }

  static auto tokenFor(LoomBinding& binding, SurfaceId surfaceId) {
    return binding.tokenFor(surfaceId);
  }

  static void publish(
      LoomBinding& binding,
      SurfaceId surfaceId,
      const std::shared_ptr<Draft>& root,
      uint64_t generation) {
    binding.publishWorkerCommit(
        surfaceId,
        {std::make_shared<LoomBinding::NodeRef>(
            surfaceId, root->tag, binding.tokenFor(surfaceId), root)},
        /*forceSync*/ true);
    (void)generation;
  }
};

namespace {

/**
 * The core with a host registered in the context container and no
 * `UIManagerDelegate` at all: the binding must reach every service through
 * the host.
 */
class LoomHeadlessBindingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    contextContainer_ = std::make_shared<ContextContainer>();
    host_ = LoomHeadlessHost::registerIn(*contextContainer_);

    ComponentDescriptorProviderRegistry providerRegistry{};
    auto registry = providerRegistry.createComponentDescriptorRegistry(
        ComponentDescriptorParameters{
            .eventDispatcher = EventDispatcher::Shared{},
            .contextContainer = contextContainer_,
            .flavor = nullptr});
    providerRegistry.add(
        concreteComponentDescriptorProvider<RootComponentDescriptor>());
    providerRegistry.add(
        concreteComponentDescriptorProvider<ViewComponentDescriptor>());

    RuntimeExecutor runtimeExecutor =
        [](std::function<void(jsi::Runtime&)>&&) {};
    uiManager_ =
        std::make_shared<UIManager>(runtimeExecutor, contextContainer_);
    uiManager_->setComponentDescriptorRegistry(std::move(registry));
    // Deliberately no setDelegate().
    binding_ = std::make_shared<LoomBinding>(uiManager_);
  }

  void TearDown() override {
    binding_.reset();
  }

  std::shared_ptr<ContextContainer> contextContainer_;
  std::shared_ptr<LoomHeadlessHost> host_;
  std::shared_ptr<UIManager> uiManager_;
  std::shared_ptr<LoomBinding> binding_;
};

TEST_F(LoomHeadlessBindingTest, CommitReachesTheRegisteredHost) {
  using Access = LoomBindingTestAccess;
  ASSERT_EQ(uiManager_->getDelegate(), nullptr);

  LoomBinding::setSurfaceConstraints(1, 200, 200, 200, 200, false, 0, 0);
  const auto token = Access::tokenFor(*binding_, 1);
  // The root's children are published; the surface root itself is the host's.
  auto child = Access::draft(
      1,
      token->generation,
      11,
      2,
      "View",
      folly::dynamic::object("width", 100.0)("height", 50.0)(
          "backgroundColor", "red"));
  child->children =
      std::make_shared<std::vector<std::shared_ptr<Access::Draft>>>();

  Access::publish(*binding_, 1, child, token->generation);
  LoomBinding::synchronizeWorkerForUIManager(*uiManager_);
  host_->drainUIQueue();

  bool created = false;
  for (const auto& recorded : host_->takeBatches()) {
    EXPECT_EQ(recorded.surfaceId, 1);
    for (const auto& operation : recorded.batch.operations) {
      if (const auto* create = std::get_if<DirectCreateOperation>(&operation);
          create != nullptr && create->tag == 2) {
        created = true;
        EXPECT_EQ(create->componentName, "View");
      }
    }
  }
  EXPECT_TRUE(created);
  EXPECT_TRUE(host_->takeFailures().empty());
}

} // namespace
} // namespace facebook::react
